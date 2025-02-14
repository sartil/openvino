// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "translate_session.hpp"

#include "helper_ops/gather_assign.hpp"
#include "helper_ops/slice_assign.hpp"
#include "input_model.hpp"
#include "openvino/op/gather.hpp"
#include "openvino/op/slice.hpp"
#include "openvino/util/common_util.hpp"
#include "openvino/util/log.hpp"
#include "place.hpp"
#include "pt_framework_node.hpp"
#include "utils.hpp"

namespace ov {
namespace frontend {
namespace pytorch {

using namespace ov::op;

TranslateSession::TranslateSession(const ov::frontend::InputModel::Ptr& input_model,
                                   const std::map<std::string, CreatorFunction>& translator_map,
                                   const std::shared_ptr<TelemetryExtension>& telemetry)
    : m_input_model(input_model),
      m_translator_map(translator_map),
      m_telemetry(telemetry),
      m_ov_model(nullptr) {}

TranslateSession::~TranslateSession() {
    if (m_telemetry) {
        // Send statistics
        for (const auto& op : m_op_statistics) {
            m_telemetry->send_event("op_count", "pytorch_" + op.first, static_cast<int>(op.second));
        }
    }
}

std::shared_ptr<ov::Model> TranslateSession::get_converted_model() {
    if (m_ov_model) {
        return m_ov_model;
    }
    m_ov_model = translate_graph(m_input_model);
    return m_ov_model;
}

std::shared_ptr<ov::Model> TranslateSession::translate_graph(const ov::frontend::InputModel::Ptr& input_model) {
    auto pytorch_model = std::dynamic_pointer_cast<pytorch::InputModel>(input_model);
    FRONT_END_GENERAL_CHECK(pytorch_model != nullptr, "Invalid input model");
    auto model = convert_pytorch_model(pytorch_model->m_model_decoder, {}, pytorch_model);
    // First delete tensor indexes from outputs then resolve input names, otherwise Parameter->Result will fail
    for (auto& result : model->get_results()) {
        auto tensor_desc = result->input_value(0);
        auto names = tensor_desc.get_names();
        if (!names.empty()) {
            auto tensor_idx = decode_tensor_name(tensor_desc);
            if (names.erase(std::to_string(tensor_idx))) {
                tensor_desc.set_names(names);
            }
        }
    }
    // Set input tensor names to be equal to signature name saved in friendly name
    for (auto& param : model->get_parameters()) {
        if (param->get_friendly_name() != param->get_name()) {
            // get_name is autogenerated name, we need to make sure that this parameter was named by frontend
            param->output(0).set_names({param->get_friendly_name()});
        }
    }
    return model;
}

std::shared_ptr<Model> TranslateSession::convert_pytorch_model(
    std::shared_ptr<TorchDecoder> pytorch_model,
    const TensorMap& external_tensor_map,
    const std::shared_ptr<pytorch::InputModel>& input_model) {
    std::shared_ptr<Model> resulting_model;  // define here to make a conversion in a nested scope
    {
        auto parameters = std::make_shared<ParameterVector>();
        auto tensor_map = std::make_shared<TensorMap>();  // tensor map of the current context
        auto mutated_tensors = std::make_shared<std::set<size_t>>();

        if (input_model) {
            // When we have input model we should use its inputs order to create Parameters
            // We use m_inputs instead of get_inputs() because latter doesn't have "self" input
            for (auto& input_p : input_model->m_inputs) {
                auto pytorch_place = std::dynamic_pointer_cast<pytorch::Place>(input_p);
                FRONT_END_GENERAL_CHECK(pytorch_place, "Only place produced by PyTorch Frontend is supported.");
                auto tensor_id = pytorch_place->get_tensor_index();
                element::Type type = pytorch_place->get_element_type();
                PartialShape pshape = pytorch_place->get_partial_shape();
                auto parameter = std::make_shared<v0::Parameter>(type, pshape);
                if (pytorch_place->get_names().size() > 0)
                    parameter->set_friendly_name(pytorch_place->get_names().at(0));
                encode_tensor_name(parameter->output(0), tensor_id);
                parameters->push_back(parameter);
                (*tensor_map)[tensor_id] = parameter;
            }
            // Add all tensors that were frozen
            for (auto& desc : input_model->m_descriptors) {
                (*tensor_map)[desc.first] = desc.second.m_value;
            }
        } else {
            // Go over all pytorch_model inputs and register them in the tensor map:
            auto inputs = pytorch_model->inputs();
            for (size_t i = 0; i < inputs.size(); ++i) {
                element::Type type = element::dynamic;
                PartialShape pshape = pytorch_model->get_input_shape(i);
                auto type_any = simplified_type_interpret(pytorch_model->get_input_type(i));
                // TODO: Use special API to set custom type specification
                if (type_any.is<element::Type>()) {
                    type = type_any.as<element::Type>();
                }
                auto parameter = std::make_shared<v0::Parameter>(type, pshape);
                parameter->set_friendly_name(pytorch_model->get_input_signature_name(i));
                encode_tensor_name(parameter->output(0), inputs.at(i), {pytorch_model->get_input_debug_name(i)});
                parameters->push_back(parameter);
                (*tensor_map)[inputs.at(i)] = parameter;
            }
        }

        auto node_visitor = [&](std::shared_ptr<TorchDecoder> node) {
            // Explore all inputs of node. Node may refer to input value that hasn't been created in the current scope.
            // But this value can be found in the outer scope, for this purpose we create new input for the model to
            // link with external scope on a higher level.

            auto raw_inputs = node->inputs();
            for (size_t i = 0; i < raw_inputs.size(); ++i) {
                auto input = raw_inputs.at(i);
                if (tensor_map->find(input) == tensor_map->end()) {
                    // Input refers value in the outer scope, need to create a new Parameter in the current scope
                    // Linkage to external scope will be performed on the level of the parent operation (if or loop)
                    // TODO: Eliminate duplication with the main code for Parameters creation
                    PartialShape ps = node->get_input_shape(i);
                    auto type = simplified_type_interpret(node->get_input_type(i));
                    auto dtype = element::dynamic;
                    if (type.is<element::Type>()) {
                        dtype = type.as<element::Type>();
                    }
                    auto parameter = std::make_shared<v0::Parameter>(dtype, ps);
                    (*tensor_map)[input] = parameter;
                    // set name of parameter to the index of node in the model
                    encode_tensor_name(parameter->output(0), input);
                    parameters->push_back(parameter);
                }
            }
            auto context = NodeContext(node, external_tensor_map, tensor_map, parameters, mutated_tensors, this);
            // Add op type in the statistics
            m_op_statistics[context.get_op_type()]++;
            auto converted_outputs = convert_node(context);

            auto fw_outputs = node->outputs();
            // Ops with subgraphs or with mutated inputs may have more outputs after conversion compared to pytorch ones
            FRONT_END_OP_CONVERSION_CHECK(fw_outputs.size() <= converted_outputs.size(),
                                          "Number of ",
                                          context.get_op_type(),
                                          " outputs greater then number of converted outputs.");

            for (size_t i = 0; i < fw_outputs.size(); ++i) {
                size_t fw_tensor_id = node->output(i);
                if (node->inputs().size() > 0 && node->may_produce_alias(0, i)) {
                    // TODO: do we need to check other inputs, not only 0?
                    auto in_tensor_id = node->inputs().at(0);
                    if (m_may_be_alias.count(fw_tensor_id)) {
                        size_t recorded_in_tensor_id;
                        std::shared_ptr<TorchDecoder> recorded_node;
                        std::tie(recorded_in_tensor_id, recorded_node, std::ignore) = m_may_be_alias.at(fw_tensor_id);
                        FRONT_END_GENERAL_CHECK(recorded_in_tensor_id == in_tensor_id,
                                                "Operation ",
                                                context.get_op_type(),
                                                " creates alias to tensor which was already created before by ",
                                                recorded_node->get_op_type(),
                                                ", but from different tensor: ",
                                                in_tensor_id,
                                                " vs ",
                                                recorded_in_tensor_id);
                    }
                    m_may_be_alias[fw_tensor_id] = {node->inputs().at(0), node, converted_outputs[i]};
                    OPENVINO_DEBUG << "Registered alias: " << fw_tensor_id << " of tensor: " << node->inputs().at(0)
                                   << " of operation: " << context.get_op_type();
                }
                FRONT_END_GENERAL_CHECK(tensor_map->find(fw_tensor_id) == tensor_map->end(),
                                        "Duplicated producer for PT value with unique ID: ",
                                        fw_tensor_id);
                auto out_type = context.get_output_type(i);
                if (out_type.is<element::Type>()) {
                    if (!converted_outputs[i].get_element_type().compatible(out_type.as<element::Type>())) {
                        OPENVINO_DEBUG << "[WARNING] Produced output type for operation " << context.get_op_type()
                                       << " for tensor id: " << fw_tensor_id << " is incompatible: produced "
                                       << converted_outputs[i].get_element_type() << " vs "
                                       << out_type.as<element::Type>();
                    }
                }
                (*tensor_map)[fw_tensor_id] = converted_outputs[i];
                encode_tensor_name(converted_outputs[i], fw_tensor_id, {node->get_output_debug_name(i)});
            }
        };

        FRONT_END_GENERAL_CHECK(pytorch_model->get_subgraph_size() == 1, "Model should have exactly 1 subgraph.");
        pytorch_model->visit_subgraph(node_visitor);

        ResultVector results;
        if (input_model) {
            // For the case when we have InputModel we need to have same order as its outputs
            for (auto& output_p : input_model->get_outputs()) {
                auto pytorch_place = std::dynamic_pointer_cast<pytorch::Place>(output_p);
                FRONT_END_GENERAL_CHECK(pytorch_place, "Only place produced by PyTorch Frontend is supported.");
                auto tensor_id = pytorch_place->get_tensor_index();
                auto ov_output = tensor_map->at(tensor_id);
                FRONT_END_GENERAL_CHECK(ov_output.get_names().size() > 0,
                                        "Tensor doesn't have name, while it should have name: ",
                                        tensor_id);
                auto result = std::make_shared<v0::Result>(ov_output);
                results.push_back(result);
            }
        } else {
            for (size_t i = 0; i < pytorch_model->num_of_outputs(); ++i) {
                size_t id = pytorch_model->output(i);
                if (tensor_map->find(id) == tensor_map->end()) {
                    // Not found in this scope, adding Parameter to connect to external scope
                    auto parameter = std::make_shared<v0::Parameter>(element::dynamic, PartialShape::dynamic());
                    encode_tensor_name(parameter->output(0), id);
                    parameters->push_back(parameter);
                    (*tensor_map)[id] = parameter;
                }
                auto ov_output = tensor_map->at(id);
                FRONT_END_GENERAL_CHECK(ov_output.get_names().size() > 0,
                                        "Tensor doesn't have name, while it should have name: ",
                                        id);
                auto result = std::make_shared<v0::Result>(ov_output);
                results.push_back(result);
            }
        }

        // Since parameters can be added we need to list all current parameters
        std::set<size_t> param_names;
        for (const auto& param : *parameters) {
            auto input_idx = decode_tensor_name(param->output(0));
            param_names.insert(input_idx);
        }
        for (const auto& tensor_id : *mutated_tensors) {
            if (param_names.count(tensor_id)) {
                FRONT_END_GENERAL_CHECK(tensor_map->count(tensor_id),
                                        "Tensor with id: ",
                                        tensor_id,
                                        " doesn't exist in tensor map.");
                // model input was mutated we need to make a result for it
                auto mutated_tensor = tensor_map->at(tensor_id);
                // empty external_tensor_map means this is main body of the model and we don't want to create
                // additional outputs in that case.
                if (!external_tensor_map.empty()) {
                    OPENVINO_DEBUG << "Creating Result for mutated tensor  " << tensor_id;
                    results.push_back(std::make_shared<v0::Result>(tensor_map->at(tensor_id)));
                }
            } else {
                OPENVINO_DEBUG << "Mutated tensor with id " << tensor_id << " doesn't exist in inputs, skipping.";
            }
        }
        resulting_model = std::make_shared<Model>(results, *parameters);
        // Did a conversion in a nested scope to automatically remove any holders of nodes except those in the graph
    }

    return resulting_model;
}

OutputVector TranslateSession::convert_node(const NodeContext& context) {
    std::string exception;
    try {
        auto it = m_translator_map.find(context.get_op_type());
        if (it != m_translator_map.end()) {
            return it->second(context);
        }
        OPENVINO_DEBUG << "No translator found for: " << context.get_op_type() << "\n";
    } catch (std::exception& e) {
        exception = e.what();
        if (m_telemetry) {
            auto cropped_message = ov::util::filter_lines_by_prefix(exception, get_pytorch_prefix());
            if (cropped_message.size()) {
                m_telemetry->send_event("error_info", cropped_message);
            }
        }
    } catch (...) {
        exception = "Unknown exception type.";
    }
    OPENVINO_DEBUG << exception << "\n";
    try {
        // Create PtFrameworkNode for everything that wasn't able to be converted normally
        return make_framework_node(context, exception);
    } catch (std::exception& e) {
        exception += " Exception happened while creating FrameworkNode with subgraphs: " + std::string(e.what());
    } catch (...) {
        exception += " Unknown exception happened while creating FrameworkNode with subgraphs";
    }
    OPENVINO_DEBUG << exception << "\n";
    return make_framework_node_ignore_bodies(context, exception);
}

void TranslateSession::encode_tensor_name(Output<Node> output,
                                          size_t tensor_idx,
                                          std::vector<std::string> additional_names) {
    if (!output.get_names().empty()) {
        OPENVINO_DEBUG << "Tensor names already exist: " << output.get_any_name() << ". Will not be rewritten with "
                       << tensor_idx << ". This is likely a mutated tensor.";
        return;
    }
    auto name = std::to_string(tensor_idx);
    std::unordered_set<std::string> names;
    names.insert(name);
    if (additional_names.size() > 0) {
        names.insert(additional_names.begin(), additional_names.end());
    }

    if (m_counter_map.count(tensor_idx)) {
        auto&& pair = m_counter_map[tensor_idx];
        auto new_name = name + '_' + std::to_string(++pair.first);
        pair.second.set_names({new_name});
        pair.second = output;
        output.set_names(names);
    } else {
        m_counter_map[tensor_idx] = {0, output};
        output.set_names(names);
    }
}

size_t TranslateSession::decode_tensor_name(const Output<Node>& output) {
    // any_name should always return numerical value even if there is a word value exist in names
    const auto& name = output.get_any_name();
    // numbers after "_" will be ignored by stoll function
    return static_cast<size_t>(std::stoll(name));
}

namespace {
Output<Node> slice_reverseprop(const Output<Node>& slice_output, const Output<Node>& value) {
    auto slice_node = slice_output.get_node_shared_ptr();
    FRONT_END_OP_CONVERSION_CHECK(ov::as_type_ptr<v8::Slice>(slice_node),
                                  "Conversion rule for aten::slice doesn't contain Slice node.");

    auto to_insert_data = slice_node->input_value(0);
    Output<Node> res;
    if (slice_node->get_input_size() == 5) {
        res = std::make_shared<SliceAssign>(to_insert_data,
                                            value,
                                            slice_node->input_value(1),
                                            slice_node->input_value(2),
                                            slice_node->input_value(3),
                                            slice_node->input_value(4));
    } else if (slice_node->get_input_size() == 4) {
        res = std::make_shared<SliceAssign>(to_insert_data,
                                            value,
                                            slice_node->input_value(1),
                                            slice_node->input_value(2),
                                            slice_node->input_value(3));
    } else {
        FRONT_END_OP_CONVERSION_CHECK(false, "Incorrect number of Slice inputs");
    }

    return res;
}

Output<Node> select_reverseprop(const Output<Node>& select_output, const Output<Node>& value) {
    auto gather_node = select_output.get_node_shared_ptr();
    FRONT_END_OP_CONVERSION_CHECK(ov::as_type_ptr<v8::Gather>(gather_node),
                                  "Conversion rule for aten::select doesn't contain Gather node.");

    auto to_insert_data = gather_node->input_value(0);
    return std::make_shared<GatherAssign>(to_insert_data,
                                          value,
                                          gather_node->input_value(1),
                                          gather_node->input_value(2));
}
}  // namespace

using ReversepropCreatorFunction = std::function<ov::Output<ov::Node>(const Output<Node>&, const Output<Node>&)>;

Output<Node> TranslateSession::get_reverseprop_op(const std::shared_ptr<TorchDecoder>& node,
                                                  const Output<Node>& direct_op_output,
                                                  const Output<Node>& value) {
    std::map<std::string, ReversepropCreatorFunction> backprop_map = {
        {"aten::slice", slice_reverseprop},
        {"aten::select", select_reverseprop},
    };

    Output<Node> backprop_node;
    try {
        auto it = backprop_map.find(node->get_op_type());
        if (it != backprop_map.end()) {
            return it->second(direct_op_output, value);
        }

    } catch (std::exception& e) {
        OPENVINO_DEBUG << "Exception happened during conversion of backprop op: " << node->get_op_type()
                       << " with schema: " << node->get_schema() << ": " << e.what();
    }
    // Create PtFrameworkNode representing unconverted backprop operation
    return std::make_shared<PtFrameworkNode>(node, OutputVector{value}, 1, true);
}

}  // namespace pytorch
}  // namespace frontend
}  // namespace ov
