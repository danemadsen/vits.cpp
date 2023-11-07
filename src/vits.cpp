#include "include/vits.h"
#include "include/debug.h"
#include "include/ggml-util.h"
#include <memory>
#include <thread>
#include <algorithm>
#include <stdlib.h>
#include <tuple>
#define VITS_DEBUG 1

vits_model::vits_model(struct ggml_context* ctx, std::unique_ptr<vits_model_data> model, int speaking_rate) {
    this->ctx = ctx;
    this->model = std::move(model);
    this->speaking_rate = speaking_rate;
    #if VITS_DEBUG
        printf("Config:\n");
        for(auto& [key, value] : this->model->config) {
            printf("  %s: %s\n", key.c_str(), value.c_str());
        }
    #endif
}

vits_model::~vits_model() {
    printf("Free'ing vits model\n");
    ggml_free(ctx);
}

template<>
std::vector<int> vits_model::load_vector_impl<int>(const std::string& serialized_data) {
    printf("Loading vector %s\n", serialized_data.c_str());
    std::vector<int> result;
    char buffer[128];
    int buffer_index = 0;

    for (int i = 0; i < serialized_data.size(); ++i) {
        char item = serialized_data[i];
        if (item == ' ' || item == '[' || item == ']') {
            continue;
        } else if (item == ',') {
            buffer[buffer_index] = '\0';
            result.push_back(std::stoi(buffer));
            buffer_index = 0;
        } else {
            buffer[buffer_index++] = item;
        }
    }

    // Handle the last item if there isn't a comma after it
    if (buffer_index > 0) {
        buffer[buffer_index] = '\0';
        result.push_back(std::stoi(buffer));
    }
    printf("Loaded vector of size %d\n", result.size());
    return result;
}

// Specialization for std::vector<int>
template<>
std::vector<std::vector<int>> vits_model::load_vector_impl<std::vector<int>>(const std::string& serialized_data_full) {
    printf("Loading vector of vectors %s\n", serialized_data_full.c_str());
    auto serialized_data = serialized_data_full.substr(1, serialized_data_full.size() - 2);
    std::vector<std::vector<int>> result;
    int i = 0;

    while (i < serialized_data.size()) {
        int start = i;
        int end = i;
        int depth = 0;
        while (end < serialized_data.size()) {
            char item = serialized_data[end];
            if (item == '[') {
                depth++;
            } else if (item == ']') {
                depth--;
            } else if (item == ',' && depth == 0) {
                break;
            }
            end++;
        }
        result.push_back(this->load_vector_impl<int>(serialized_data.substr(start, end - start)));
        i = end + 1;
    }

    printf("Loaded vector of size %d\n", result.size());
    return result;
};

std::string vits_model::load_param(std::string key) {
    printf("Loading param for key: %s\n", key.c_str());
    auto val_str = this->model->config[key];
    if (val_str.empty())
        throw std::runtime_error("Failed to find '" + key + "' in the model's config");
    return val_str;
}

int vits_model::load_number(std::string key) {
    auto value = std::stoi(this->load_param(key));
    printf("%s = %d\n", key.c_str(), value);
    return value;
}

float vits_model::load_float(std::string key) {
    auto value = std::stof(this->load_param(key));
    printf("%s = %f\n", key.c_str(), value);
    return value;
}


//https://github.com/huggingface/transformers/blob/09b2de6eb74b1e5ff4f4c3d9839485f4165627c9/src/transformers/models/vits/modeling_vits.py#L1356

struct ggml_tensor* layer_norm(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* weight, struct ggml_tensor* bias, float eps=1e-5) {
    auto cur = ggml_norm(ctx, input, eps);
    cur = ggml_mul(ctx, cur, weight);
    cur = ggml_add(ctx, cur, bias);
    return cur;
}

struct ggml_tensor* linear_with_bias(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* weight, struct ggml_tensor* bias) {
    auto cur = ggml_mul_mat(ctx, weight, input);
    cur = ggml_add(ctx, cur, bias);
    return cur;
}

struct ggml_tensor* conv1d(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* proj_weights, int stride = 1, int padding = 0, int dilation= 1) {
    ASSERT(input->n_dims == 3, "Conv only supported on 3d tensors");
    auto proj_weights_fp16 = cast_tensor_fp32_to_fp16(ctx, proj_weights);
    return ggml_conv_1d(ctx, proj_weights_fp16, input, stride, padding, dilation);
}

struct ggml_tensor* depthwise_conv_with_bias(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* proj_weights, struct ggml_tensor* proj_bias, int stride = 1, int padding = 0, int dilation= 1) {
    ASSERT(input->n_dims == 3, "Depth conv only supported on 3d tensors");
    auto groups = proj_weights->ne[2];
    auto proj_weights_fp16 = cast_tensor_fp32_to_fp16(ctx, proj_weights);
    struct ggml_tensor* final = nullptr;

    // Each group is a 1d conv applied to a slice of the input. Depth-wise convolution
    printf("Depthwise Convolution with %d groups\n", groups);
    PRINT_MEM(ctx)
    for(int i = 0; i < groups; i++) {
        auto input_i = slice_3d(ctx, input, 0, -1, i, (i + 1), 0, -1);
        auto weights_i = slice_3d(ctx, proj_weights_fp16, 0, -1, 0, -1, i, (i + 1));

        auto result = ggml_conv_1d(ctx, weights_i, input_i, stride, padding, dilation);
        result = ggml_reshape_3d(ctx, result, result->ne[0], result->ne[1], result->ne[2]);
        if (final != nullptr) {
            final = concat_3d(ctx, final, result, 1);
        } else {
            final = result;
        }
    }
    auto cur = ggml_permute(ctx, final, 1, 0, 2, 3);
    cur = ggml_cont(ctx, cur);
    cur = ggml_add(ctx, cur, proj_bias);
    cur = ggml_permute(ctx, cur, 1, 0, 2, 3);
    cur = ggml_cont(ctx, cur);
    return cur;
}

struct ggml_tensor* conv1d_with_bias(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* proj_weights, struct ggml_tensor* proj_bias, int stride = 1, int padding = 0, int dilation= 1) {
    auto cur = conv1d(ctx, input, proj_weights, stride, padding, dilation);
    cur = ggml_permute(ctx, cur, 1, 0, 2, 3);
    cur = ggml_cont(ctx, cur);
    cur = ggml_add(ctx, cur, proj_bias);
    cur = ggml_permute(ctx, cur, 1, 0, 2, 3);
    cur = ggml_cont(ctx, cur);
    cur = ggml_reshape_3d(ctx, cur, cur->ne[0], cur->ne[1], cur->ne[2]);
    return cur;
}

struct ggml_tensor* conv_transpose_1d_with_bias(struct ggml_context* ctx, struct ggml_tensor* input, struct ggml_tensor* proj_weights, struct ggml_tensor* proj_bias, int stride = 1, int padding = 0, int dilation= 1) {
    ASSERT(input->n_dims == 3, "Depth conv only supported on 3d tensors");
    ASSERT(dilation == 1, "Dilation not supported");

    auto kernel_size = proj_weights->ne[0];
    auto batch_size = input->ne[2];
    auto in_channels = input->ne[1];
    auto in_length = input->ne[0];
    auto out_channels = proj_weights->ne[1];
    ASSERT(batch_size == 1, "Batch size must be 1");

    SHAPE(input)
    printf("Conv1DTranspose kernel_size = %d, stride = %d, padding = %d, dilation = %d\n", kernel_size, stride, padding, dilation);
    PRINT_MEM(ctx)

    struct ggml_tensor* expanded_input = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, in_length * stride, in_channels, batch_size);
    for (int i = 0; i < in_length; i++) {
        /*auto section = concat_3d(
                ctx,
                slice_3d(ctx, input, i, i+1, 0, -1, 0, -1),
                ggml_new_tensor_3d(ctx, GGML_TYPE_F32, stride, in_channels, batch_size),
                0
        );
        if (expanded_input == nullptr) {
            expanded_input = section;
        } else {
            expanded_input = concat_3d(ctx, expanded_input, section, 0);
        }*/
        /*expanded_input = ggml_set(
            ctx,
            expanded_input,
            ggml_view_3d(ctx, input, 1, in_channels, batch_size, in_length, in_channels, i * in_channels * batch_size *
                    ggml_element_size(input)),
            expanded_input->nb[1],
            expanded_input->nb[2],
            expanded_input->nb[3],
            i * stride * in_channels * batch_size * ggml_element_size(expanded_input)
        );*/
    }
    SHAPE(expanded_input)
    PRINT_MEM(ctx)
    auto flipped_weights = proj_weights;//flip_3d(ctx, proj_weights, 0)
    auto flipped_transposed_weights = ggml_permute(ctx, flipped_weights, 0, 2, 1, 3);
    SHAPE(flipped_transposed_weights);
    auto cur = conv1d_with_bias(ctx, expanded_input, flipped_transposed_weights, proj_bias, 1, kernel_size - 1);
    PRINT_MEM(ctx)
    auto start = padding;
    auto end = stride > 1 ? -(padding + stride - 1) - 1 : -1;
    auto sliced = slice_3d(ctx, cur, start, end, 0, -1, 0, -1);
    SHAPE(sliced);
    return sliced;

}

struct ggml_tensor * get_relative_embeddings(struct ggml_context* ctx, struct ggml_tensor * relative_embeddings, int length, int window_size) {
    int pad_length = std::max(length - (window_size + 1), 0);
    struct ggml_tensor* padded_embeddings = relative_embeddings;
    if (pad_length > 0) {
        padded_embeddings = pad_3d(ctx, relative_embeddings, {0, 0, pad_length, pad_length, 0, 0});
    }

    int slice_start_position = std::max((window_size + 1) - length, 0);
    int slice_end_position = slice_start_position + 2 * length - 1;
    printf("slice_start_position = %d, slice_end_position = %d\n", slice_start_position, slice_end_position);
    return slice_3d(ctx, padded_embeddings, 0, -1, slice_start_position, slice_end_position, 0, -1);
}

struct ggml_tensor * relative_position_to_absolute_position(struct ggml_context* ctx, struct ggml_tensor * x) {
    auto sizes = x->ne;
    auto batch_heads = sizes[2];
    auto length = sizes[1];

    x = pad_3d(ctx, x, {0, 0, 0, 0, 0, 1});
    printf("length = %d, batch_heads = %d\n", length, batch_heads);

    auto x_flat = ggml_reshape_2d(ctx, x, length * 2 * length, batch_heads);
    x_flat = pad_2d(ctx, x_flat, {0, 0, 0, (int)length - 1});

    auto x_final = ggml_reshape_3d(ctx, x_flat, 2 * length - 1, length + 1, batch_heads);
    x_final = slice_3d(ctx, x_final, length - 1, -1, 0, length, 0, -1);
    return x_final;
}

struct ggml_tensor * absolute_position_to_relative_position(struct ggml_context* ctx, struct ggml_tensor * x) {
    auto sizes = x->ne;
    auto batch_heads = sizes[2];
    auto length = sizes[1];

    x = pad_3d(ctx, x, {0, 0, 0, 0, 0, (int)length - 1});
    auto x_flat = ggml_reshape_2d(ctx, x, length * length + length * (length - 1), batch_heads);
    x_flat = pad_2d(ctx, x_flat, {0, 0, (int)length, 0});

    auto x_final = ggml_reshape_3d(ctx, x_flat, 2 * length, length, batch_heads);
    x_final = slice_3d(ctx, x_final, 1, -1, 0, -1, 0, -1);

    return x_final;
}

ggml_tensor* shape_attn(struct ggml_context* ctx, struct ggml_tensor* tensor, int head_dim, int num_heads, int seq_len) {
    // bsz is always 1
    auto cur = ggml_reshape_4d(ctx, tensor, head_dim, num_heads, seq_len, 1);
    cur = ggml_permute(ctx, cur, 0, 2, 1, 3);
    return ggml_cont(ctx, cur);
}

struct std::tuple<ggml_tensor*, ggml_tensor*, ggml_tensor*> vits_model::text_encoder_graph(struct ggml_tensor* input_ids) {
    auto config = this->model->config;
    auto act_func = this->load_param("hidden_act");
    auto hidden_size = this->load_number("hidden_size");
    auto window_size = this->load_number("window_size");
    auto flow_size = this->load_number("flow_size");
    auto layer_head_mask = false;//config["layer_head_mask"];
    auto layer_count = this->load_number("num_hidden_layers");
    auto ffn_kernel_size = this->load_number("ffn_kernel_size");
    auto num_heads = this->load_number("num_attention_heads");
    auto layer_norm_eps = this->load_float("layer_norm_eps");
    auto head_dim = hidden_size / num_heads;
    auto embed_dim = hidden_size;

    struct ggml_tensor* cur = nullptr;

    auto _0 = model->use("text_encoder");

    SHAPE(input_ids);
    cur = ggml_get_rows(ctx, model->get("embed_tokens.weight"), input_ids);
    cur = ggml_scale(ctx, cur, ggml_new_f32(ctx, (float)sqrt(hidden_size)));

    for (int i = 0; i < layer_count; i++) {
        std::string base_name = "encoder.layers." + std::to_string(i);
        auto _1 = model->use(base_name);
        auto residual = cur;
        // Attention
        {
            printf("Building '%d'\n", i);
            auto _ = model->use("attention");

            auto emb_rel_k = model->get("emb_rel_k");
            auto emb_rel_v = model->get("emb_rel_v");
            auto k_proj_w = model->get("k_proj.weight");
            auto k_proj_b = model->get("k_proj.bias");
            auto v_proj_w = model->get("v_proj.weight");
            auto v_proj_b = model->get("v_proj.bias");
            auto q_proj_w = model->get("q_proj.weight");
            auto q_proj_b = model->get("q_proj.bias");
            auto out_proj_w = model->get("out_proj.weight");
            auto out_proj_b = model->get("out_proj.bias");

            // 1. Project the input into query, key, and value states.
            auto query = linear_with_bias(ctx, cur, q_proj_w, q_proj_b);
            auto key = linear_with_bias(ctx, cur, k_proj_w, k_proj_b);
            auto value = linear_with_bias(ctx, cur, v_proj_w, v_proj_b);

            int bsz = cur->ne[2];
            int tgt_len = query->ne[1];
            int src_len = key->ne[1];
            printf("bsz = %d, tgt_len = %d, src_len = %d\n", bsz, tgt_len, src_len);
            printf("num_heads = %d, head_dim = %d\n", num_heads, head_dim);

            // Scaling the query_states (Assuming `scaling` is a float or double type variable you have)
            float scaling = pow(head_dim, -0.5);
            query = ggml_scale(ctx, query, ggml_new_f32(ctx, scaling));
            auto query_states = shape_attn(ctx, query, head_dim, num_heads, tgt_len);
            auto key_states = shape_attn(ctx, key, head_dim, num_heads, value->ne[1]);
            auto value_states = shape_attn(ctx, value, head_dim, num_heads, value->ne[1]);

            ASSERT(cur->ne[2] == 1, "Batch size must be 1");

            int dim0 = bsz * num_heads;
            int dim2 = head_dim;
            int dim1 = ggml_nelements(query) / (dim0 * dim2);
            printf("dim0 = %d, dim1 = %d, dim2 = %d\n", dim0, dim1, dim2);
            ASSERT(src_len == dim1, "Shape is wrong");

            // ggml shapes are flipped. hf shape is (bsz * self.num_heads, -1, self.head_dim)
            int target_shape[3] = {dim2, dim1, dim0};
            query_states = ggml_reshape_3d(ctx, query_states, target_shape[0], target_shape[1], target_shape[2]);

            key_states = ggml_reshape_3d(ctx, key_states, target_shape[0], target_shape[1], target_shape[2]);
            value_states = ggml_reshape_3d(ctx, value_states, target_shape[0], target_shape[1], target_shape[2]);

            auto attn_weights = ggml_mul_mat(ctx, key_states, query_states);

            ASSERT(attn_weights->ne[2] == dim0 && attn_weights->ne[1] == src_len && attn_weights->ne[0] == src_len, "Shape is wrong");

            if (window_size > 0) {
                auto key_relative_embeddings = get_relative_embeddings(ctx, emb_rel_k, src_len, window_size);
                auto relative_logits = ggml_mul_mat(ctx, key_relative_embeddings, query_states);
                auto rel_pos_bias = relative_position_to_absolute_position(ctx, relative_logits);

                attn_weights = ggml_add(ctx, attn_weights, rel_pos_bias);
            }

            attn_weights = ggml_soft_max(ctx,attn_weights);

            // If layer head mask is defined
            if (layer_head_mask) {
                ASSERT(false, "Not implemented");
            }

            value_states = ggml_permute(ctx, value_states, 1, 0, 2, 3);
            value_states = ggml_cont(ctx, value_states);

            auto attn_output = ggml_mul_mat(ctx, value_states, attn_weights);

            ASSERT(attn_output->ne[0] == dim2 && attn_output->ne[1] == tgt_len && attn_output->ne[2] == dim0, "`attn_output` size mismatch");

            if (window_size > 0) {
                auto value_relative_embeddings = get_relative_embeddings(ctx, emb_rel_v, src_len, window_size);
                auto relative_weights = absolute_position_to_relative_position(ctx, attn_weights);
                value_relative_embeddings = ggml_permute(ctx, value_relative_embeddings, 1, 0, 2, 3);
                value_relative_embeddings = ggml_cont(ctx, value_relative_embeddings);
                auto rel_pos_bias = ggml_mul_mat(ctx, value_relative_embeddings, relative_weights);
                attn_output = ggml_add(ctx, attn_output, rel_pos_bias);
            }


            attn_output = ggml_reshape_4d(ctx, attn_output, head_dim, tgt_len, num_heads, 1);
            attn_output = ggml_permute(ctx, attn_output, 0, 2, 1, 3);
            attn_output = ggml_cont(ctx, attn_output);
            attn_output = ggml_reshape_3d(ctx, attn_output, embed_dim, tgt_len, 1);

            cur = linear_with_bias(ctx, attn_output, out_proj_w, out_proj_b);
        }

        printf("Layer norm for layer %d\n", i);

        // Layer norm
        {
            auto _ = model->use("layer_norm");
            cur = ggml_add(ctx, cur, residual);
            cur = layer_norm(ctx, cur, model->get("weight"), model->get("bias"), layer_norm_eps);

        }

        residual = cur;
        printf("FF for layer %d\n", i);
        //Feed forward
        {
            auto _ = model->use("feed_forward");
            if (act_func != "relu")
                throw std::runtime_error("activation function not supported " + act_func);

            cur = ggml_permute(ctx, cur, 1, 0, 2, 3);

            int pad_left = 0;
            int pad_right = 0;

            if (ffn_kernel_size > 1) {
                pad_left = (ffn_kernel_size - 1) / 2;
                pad_right = ffn_kernel_size / 2;
            } else {
                throw std::runtime_error("ffn_kernel_size == 1 not supported ");
            }

            cur = pad_3d(ctx, cur, {0, 0, 0, 0, pad_left, pad_right});

            cur = conv1d_with_bias(ctx, cur,  model->get("conv_1.weight"), model->get("conv_1.bias"));
            cur = ggml_relu(ctx, cur);

            cur = ggml_reshape_3d(ctx, cur, cur->ne[0], cur->ne[1], 1);
            cur = pad_3d(ctx, cur, {0, 0, 0, 0, pad_left, pad_right});

            cur = conv1d_with_bias(ctx, cur,  model->get("conv_2.weight"), model->get("conv_2.bias"));
            cur = ggml_reshape_3d(ctx, cur, cur->ne[0], cur->ne[1], 1);

            cur = ggml_permute(ctx, cur, 1, 0, 2, 3);
        }


        printf("Final layer norm for layer %d\n", i);
        // Final layer norm
        {
            auto _ = model->use("final_layer_norm");
            cur = ggml_cont(ctx, cur);
            cur = ggml_add(ctx, cur, residual);
            cur = layer_norm(ctx, cur, model->get("weight"), model->get("bias"), layer_norm_eps);
        }

    }
    printf("Calculating prior and variances\n");
    // In the future add support for returning this
    printf("Final proj for text encoder\n");
    auto text_encoder_output = cur;

    cur = ggml_permute(ctx, text_encoder_output, 1, 0, 2, 3);
    cur = ggml_cont(ctx, cur);

    auto stats = conv1d_with_bias(ctx, cur, model->get("project.weight"), model->get("project.bias"));
    stats = ggml_permute(ctx, stats, 1, 0, 2, 3);
    stats = ggml_view_3d(ctx, stats, stats->ne[0], stats->ne[1], stats->ne[2], stats->ne[1], stats->ne[2], 0);
    stats = ggml_cont(ctx, stats);

    auto [prior_means, prior_log_variances] = split_3d(ctx, stats, flow_size, flow_size, 0);
    printf("Finished text encoder\n");

    return std::make_tuple(text_encoder_output, prior_means, prior_log_variances);
}

struct ggml_tensor* add_tanh_sigmoid_multiply(struct ggml_context* ctx, struct ggml_tensor* a, struct ggml_tensor* b, int num_channels) {
    auto in_act = ggml_add(ctx, a, b);
    auto in_act_slice = slice_3d(ctx, in_act, 0, -1, 0, num_channels, 0, -1);
    auto tanh = ggml_tanh(ctx, in_act_slice);
    auto sigmoid = ggml_sigmoid(ctx, in_act_slice);
    auto out = ggml_mul(ctx, tanh, sigmoid);
    return out;
}

struct ggml_tensor* vits_model::wavenet_graph(struct ggml_tensor* inputs, struct ggml_tensor* global_conditioning) {
    auto num_layers = this->load_number("prior_encoder_num_wavenet_layers");
    auto hidden_size = this->load_number("hidden_size");
    auto wavenet_dilation_rate = this->load_number("wavenet_dilation_rate");
    auto wavenet_kernel_size = this->load_number("wavenet_kernel_size");
    auto speaker_embedding_size = this->load_number("speaker_embedding_size");

    auto _ = model->use("wavenet");
    auto outputs = ggml_new_tensor(ctx, inputs->type, inputs->n_dims, inputs->ne);
    ASSERT(global_conditioning == nullptr, "Not implemented");

    for (int i = 0; i < num_layers; ++i) {
        struct ggml_tensor* global_states = nullptr;
        {
            struct ggml_tensor* acts = nullptr;
            {
                auto _0 = model->use("in_layers." + std::to_string(i));
                auto dilation = (int) pow(wavenet_dilation_rate, i);
                auto padding = (int) ((wavenet_kernel_size * dilation - dilation) / 2);

                auto hidden_states = conv1d_with_bias(ctx, inputs, this->model->get("weight"), this->model->get("bias"), 1, padding, dilation);
                if (global_conditioning != nullptr) {
                    ASSERT(false, "Global conditioning not implemented");
                } else {
                    global_states = ggml_new_tensor(ctx, inputs->type, inputs->n_dims, inputs->ne);
                }

                acts = add_tanh_sigmoid_multiply(ctx, hidden_states, global_states, hidden_size);
            }

            {
                auto _0 = model->use("res_skip_layers." + std::to_string(i));

                auto res_skip_acts = conv1d_with_bias(ctx, acts, this->model->get("weight"), this->model->get("bias"));
                if (i < num_layers -1) {
                    auto res_skip_acts_slice = slice_3d(ctx, res_skip_acts, 0, -1, 0, hidden_size, 0, -1);
                    inputs = ggml_add(ctx, inputs, res_skip_acts_slice);

                    auto res_skip_acts_slice_outputs = slice_3d(ctx, res_skip_acts, 0, -1, hidden_size, -1, 0, -1);
                    outputs = ggml_add(ctx, outputs, res_skip_acts_slice_outputs);
                } else {
                    outputs = ggml_add(ctx, outputs, res_skip_acts);
                }
            }
        }
    }
    return outputs;
}

struct ggml_tensor* vits_model::flow_graph(struct ggml_context* ctx, struct ggml_tensor* inputs, struct ggml_tensor* conditioning, bool reverse) {
    ASSERT(reverse, "Non reverse not supported");

    auto _0 = model->use("flow");
    auto num_flows = this->load_number("prior_encoder_num_flows");
    struct ggml_tensor* cur = inputs;

    if (!reverse) {
        ASSERT(false, "Non reverse not supported");
    } else {

        // flow(inputs, padding_mask, global_conditioning, reverse=True)
        auto half_channels = this->load_number("flow_size") / 2;
        for(int i = num_flows-1; i > -1; --i) {
            auto _1 = model->use("flows." + std::to_string(i));
            {

                auto [first_half, second_half] = split_3d(ctx, inputs, half_channels, half_channels, 1);
                auto hidden_states = conv1d_with_bias(ctx, first_half, this->model->get("conv_pre.weight"),
                                                      this->model->get("conv_pre.bias"));

                hidden_states = this->wavenet_graph(hidden_states, conditioning);
                auto mean = conv1d_with_bias(ctx, hidden_states, this->model->get("conv_post.weight"),
                                             this->model->get("conv_post.bias"));

                if (!reverse) {
                    ASSERT(false, "Non reverse not supported");
                } else {
                    second_half = ggml_sub(ctx, second_half, mean);
                    cur = concat_3d(ctx, first_half, second_half, 1);
                }
            }
            cur = flip_3d(ctx, cur, 1);
        }
    }
    return cur;
}


int get_padding_hifigan_residual_block(int kernel_size, int dilation=1) {
    return (int) ((kernel_size * dilation - dilation) / 2);
}

struct ggml_tensor* vits_model::hifigan_residual_block_graph(struct ggml_context *ctx, struct ggml_tensor *hidden_states, int kernel_size, std::vector<int> dilation, double leaky_relu_slope) {
    for (int i = 0; i < dilation.size(); i++) {
        auto residual = hidden_states;
        auto cur = hidden_states;
        {
            auto _0 = model->use("convs1." + std::to_string(i));
            cur = leaky_relu(ctx, cur, leaky_relu_slope);
            cur = conv1d_with_bias(ctx, cur,
                                   this->model->get("weight"),
                                   this->model->get("bias"),
                                   1,
                                   get_padding_hifigan_residual_block(kernel_size, dilation[i]),
                                   dilation[i]
            );
        }

        {
            auto _0 = model->use("convs2." + std::to_string(i));
            cur = leaky_relu(ctx, cur, leaky_relu_slope);
            cur = conv1d_with_bias(ctx, cur,
                                   this->model->get("weight"),
                                   this->model->get("bias"),
                                   1,
                                   get_padding_hifigan_residual_block(kernel_size, 1),
                                   1
            );
        }
        hidden_states = ggml_add(ctx, cur, residual);
    }
    return hidden_states;
}

struct ggml_tensor* vits_model::hifigan_graph(struct ggml_context* ctx, struct ggml_tensor * spectogram, struct ggml_tensor* global_conditioning) {
    auto _ = model->use("decoder");
    auto upsample_rates = this->load_vector<int>("upsample_rates");
    auto upsample_kernel_sizes = this->load_vector<int>("upsample_kernel_sizes");
    auto num_upsamples = upsample_rates.size();
    auto kernel_sizes = this->load_vector<int>("resblock_kernel_sizes");
    auto num_kernels = kernel_sizes.size();
    auto dilations = this->load_vector<std::vector<int>>("resblock_dilation_sizes");
    auto leaky_relu_slope = this->load_float("leaky_relu_slope");
    std::vector<std::tuple<int, std::vector<int>, double>> all_params;

    for (int i = 0; i < num_upsamples; ++i) {
        auto channels = this->load_number("upsample_initial_channel") / (int) pow(2, i + 1);
        for (int j = 0; j < kernel_sizes.size(); ++j) {
            all_params.push_back(std::make_tuple(kernel_sizes[j], dilations[j], leaky_relu_slope));
        }
    }


    auto hidden_states = conv1d_with_bias(ctx, spectogram, this->model->get("conv_pre.weight"), this->model->get("conv_pre.bias"), 1, 3);

    if (global_conditioning != nullptr) {
        ASSERT(false, "Not implemented");
    }

    for(int i = 0; i < num_upsamples; ++i)
    {
        {
            auto _0 = model->use("upsampler." + std::to_string(i));
            hidden_states = leaky_relu(ctx, hidden_states, leaky_relu_slope);
            auto padding = (int) ((upsample_kernel_sizes[i] - upsample_rates[i]) / 2);

            hidden_states = conv_transpose_1d_with_bias(ctx, hidden_states, this->model->get("weight"), this->model->get("bias"), upsample_rates[i], padding, 1);

        }

        {
            auto idx = i * num_kernels;
            const auto [kernel_size, dilation, slope] = all_params[idx];
            struct ggml_tensor* res_state = nullptr;
            {
                auto _0 = model->use("resblocks." + std::to_string(idx));
                res_state = this->hifigan_residual_block_graph(ctx, hidden_states, kernel_size, dilation, slope);
            }

            for (auto j = 1; j < num_kernels; ++j) {
                idx = i * num_kernels + j;
                auto _0 = model->use("resblocks." + std::to_string(idx));
                auto [kernel_size, dilation, slope] = all_params[idx];
                auto block_res = this->hifigan_residual_block_graph(ctx, hidden_states, kernel_size, dilation, slope);
                res_state = ggml_add(ctx, res_state, block_res);
            }
            hidden_states = ggml_scale(ctx, res_state, ggml_new_f32(ctx, (float) (1.0 / num_kernels)));
        }
    }

    hidden_states = leaky_relu(ctx, hidden_states, leaky_relu_slope);
    hidden_states = conv1d(ctx, hidden_states, this->model->get("conv_post.weight"), 1, 3);
    auto waveform = ggml_tanh(ctx, hidden_states);
    return waveform;
}

struct ggml_tensor* vits_model::dilated_depth_separable_conv_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning) {
    printf("Dilated depth separable conv\n");
    auto kernel_size = this->load_number("duration_predictor_kernel_size");
    auto num_layers = this->load_number("depth_separable_num_layers");

    if (global_conditioning != nullptr)
        inputs = ggml_add(ctx, inputs, global_conditioning);

    inputs = ggml_reshape_3d(ctx, inputs, inputs->ne[0], inputs->ne[1], inputs->ne[2]);
    for(int i = 0; i < num_layers; ++i) {
        auto conv_dilated_i_weight = this->model->get("convs_dilated." + std::to_string(i) + ".weight");
        auto conv_dilated_i_bias = this->model->get("convs_dilated." + std::to_string(i) + ".bias");
        auto dilation = pow(kernel_size, i);
        auto padding = (int) ((kernel_size * dilation - dilation) / 2);

        auto hidden_states = depthwise_conv_with_bias(ctx, inputs, conv_dilated_i_weight, conv_dilated_i_bias, 1, padding, dilation);

        auto norm1_i_weight = this->model->get("norms_1." + std::to_string(i) + ".weight");
        auto norm1_i_bias = this->model->get("norms_1." + std::to_string(i) + ".bias");
        hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);
        hidden_states = layer_norm(ctx, hidden_states, norm1_i_weight, norm1_i_bias);
        hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);

        hidden_states = ggml_gelu(ctx, hidden_states);

        auto conv_pointwise_i_weight = this->model->get("convs_pointwise." + std::to_string(i) + ".weight");
        auto conv_pointwise_i_bias = this->model->get("convs_pointwise." + std::to_string(i) + ".bias");
        hidden_states = conv1d_with_bias(ctx, hidden_states, conv_pointwise_i_weight, conv_pointwise_i_bias);

        auto norm2_i_weight = this->model->get("norms_2." + std::to_string(i) + ".weight");
        auto norm2_i_bias = this->model->get("norms_2." + std::to_string(i) + ".bias");
        hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);
        hidden_states = layer_norm(ctx, hidden_states, norm2_i_weight, norm2_i_bias);
        hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);

        hidden_states = ggml_gelu(ctx, hidden_states);
        inputs = ggml_add(ctx, inputs, hidden_states);
    }
    return inputs;
}


struct ggml_tensor* rational_quadratic_spline(
        struct ggml_context* ctx,
        struct ggml_tensor* inputs,
        struct ggml_tensor* unnormalized_widths,
        struct ggml_tensor* unnormalized_heights,
        struct ggml_tensor* unnormalized_derivatives,
        bool reverse = false,
        float tail_bound = 5.0,
        float min_bin_width = 1e-3,
        float min_bin_height = 1e-3,
        float min_derivative = 1e-3)
{
    ASSERT(reverse, "Non reverse not supported");
    printf("Rational quadratic spline\n");

    auto upper_bound = tail_bound;
    auto lower_bound = -tail_bound;

    auto num_bins = unnormalized_widths->ne[unnormalized_widths->n_dims-1];

    ASSERT(min_bin_width * num_bins <= 1.0, ("Minimal bin width " + std::to_string(min_bin_width) + " too large for the number of bins " + std::to_string(num_bins)).c_str());
    ASSERT(min_bin_height * num_bins <= 1.0, ("Minimal bin height " + std::to_string(min_bin_height) + " too large for the number of bins " + std::to_string(num_bins)).c_str());

    auto widths = ggml_soft_max(ctx, unnormalized_widths);
    widths = ggml_scale(ctx, widths, ggml_new_f32(ctx, min_bin_width + (1 - min_bin_width * num_bins)));
    auto cumwidths = cumsum(ctx, widths);

    cumwidths = pad_3d(ctx, cumwidths, {0, 0, 0, 0, 1, 0});
    cumwidths = ggml_add(ctx, ggml_scale(ctx, cumwidths, ggml_new_f32(ctx, upper_bound - lower_bound)), tensor_like(ctx, cumwidths, lower_bound));
    cumwidths = index_put_last_dim(ctx, cumwidths, 0, lower_bound);
    cumwidths = index_put_last_dim(ctx, cumwidths, -1, upper_bound);

    widths = ggml_sub(ctx,
      slice_3d(ctx, cumwidths, 1, -1, 0, -1, 0, -1),
      slice_3d(ctx, cumwidths, 0, -2, 0, -1, 0, -1)
    );

    auto derivatives = ggml_add(ctx, softplus(ctx, unnormalized_derivatives), tensor_like(ctx, unnormalized_derivatives, min_derivative));

    auto heights = ggml_soft_max(ctx, unnormalized_heights);
    heights = ggml_add(ctx, ggml_scale(ctx, heights, ggml_new_f32(ctx, (1 - min_bin_height * num_bins))), tensor_like(ctx, heights, min_bin_height));
    auto cumheights = cumsum(ctx, heights);

    cumheights = pad_3d(ctx, cumheights, {0, 0, 0, 0, 1, 0});
    cumheights = ggml_add(ctx, ggml_scale(ctx, cumheights, ggml_new_f32(ctx, upper_bound - lower_bound)), tensor_like(ctx, cumheights, lower_bound));
    cumheights =index_put_last_dim(ctx, cumheights, 0, lower_bound);
    cumheights = index_put_last_dim(ctx, cumheights, -1, upper_bound);
    heights = ggml_sub(ctx,
                       slice_3d(ctx, cumheights, 1, -1, 0, -1, 0, -1),
                       slice_3d(ctx, cumheights, 0, -2, 0, -1, 0, -1)
    );


    auto bin_locations = reverse ? cumheights : cumwidths;

    bin_locations = index_add_last_dim(ctx, bin_locations, -1, 1e-6);

    inputs = ggml_reshape_2d(ctx, inputs, 1, inputs->ne[0]);
    bin_locations = ggml_reshape_2d(ctx, bin_locations, bin_locations->ne[0], bin_locations->ne[1]);
    auto bin_idx_cmp = tensor_compare(ctx, inputs, bin_locations, [](float a, float b) { return a >= b; });
    auto bin_idx_sum_rows = ggml_sum_rows(ctx, bin_idx_cmp);
    auto bin_idx = ggml_sub(
            ctx,
            ggml_sum_rows(ctx, bin_idx_cmp),
            tensor_like(ctx, bin_idx_sum_rows, 1)
    );
    bin_idx = ggml_reshape_1d(ctx, bin_idx, bin_idx->ne[1]);


    auto input_cumwidths = tensor_gather(ctx, cumwidths, 0, bin_idx);
    auto input_bin_widths = tensor_gather(ctx, widths, 0, bin_idx);
    auto input_cumheights = tensor_gather(ctx, cumheights, 0, bin_idx);

    auto delta = ggml_div(ctx, heights, widths);
    auto input_delta = tensor_gather(ctx, delta, 0, bin_idx);

    auto input_derivatives = tensor_gather(ctx, derivatives, 0, bin_idx);
    auto input_derivatives_plus_one = tensor_gather(ctx, slice_3d(ctx, derivatives, 0, 1, 0, -1, 0, -1), 0, bin_idx);

    auto input_heights = tensor_gather(ctx, heights, 0, bin_idx);
    auto intermediate1 = ggml_sub(ctx, ggml_add(ctx, input_derivatives, input_derivatives_plus_one), ggml_scale(ctx, input_delta, ggml_new_f32(ctx, 2)));
    struct ggml_tensor* outputs = nullptr;

    if (!reverse) {
        ASSERT(false, "Non reverse not supported");
    } else {
        inputs = ggml_reshape_1d(ctx, inputs, inputs->ne[1]);
        auto intermediate2 = ggml_sub(ctx, inputs, input_cumheights);
        auto intermediate3 = ggml_mul(ctx, intermediate2, intermediate1);

        auto a = ggml_add(ctx, ggml_mul(ctx, input_heights, ggml_sub(ctx, input_delta, input_derivatives)), intermediate3);
        auto b = ggml_sub(ctx, ggml_mul(ctx, input_heights, input_derivatives), intermediate3);
        auto c = ggml_mul(ctx, ggml_neg(ctx, input_delta), intermediate2);

        auto discriminant = ggml_sub(ctx, tensor_pow(ctx, b, 2), ggml_mul(ctx, ggml_scale(ctx, a, ggml_new_f32(ctx, 4)), c));

        auto root = ggml_div(ctx,
                             ggml_scale(ctx, c, ggml_new_f32(ctx, 2)),
                             ggml_sub(ctx, ggml_neg(ctx, b), ggml_sqrt(ctx, discriminant))
        );

        outputs = ggml_add(ctx, ggml_mul(ctx, root, input_bin_widths), input_cumwidths);
        ASSERT(outputs->n_dims == 1, "outputs size mismatch");
        outputs = ggml_reshape_3d(ctx, outputs, outputs->ne[0], 1, 1);
    }
    return outputs;
}

struct ggml_tensor* unconstrained_rational_quadratic_spline(
        struct ggml_context* ctx,
        struct ggml_tensor* inputs,
        struct ggml_tensor* unnormalized_widths,
        struct ggml_tensor* unnormalized_heights,
        struct ggml_tensor* unnormalized_derivatives,
        bool reverse = false,
        float tail_bound = 5.0,
        float min_bin_width = 1e-3,
        float min_bin_height = 1e-3,
        float min_derivative = 1e-3)
{
    ASSERT(reverse, "Non reverse not supported");
    printf("Unconstrained rational quadratic spline\n");
    auto inputs_more_than_min = tensor_compare(ctx, inputs, tensor_like(ctx, inputs, -tail_bound), [] (auto a, auto b) { return a >= b; });
    auto inputs_less_than_max = tensor_compare(ctx, inputs, tensor_like(ctx, inputs, tail_bound) , [] (auto a, auto b) { return a <= b; });

    auto inside_interval_mask = ggml_mul(ctx, inputs_less_than_max, inputs_more_than_min);
    auto outside_interval_mask = tensor_not(ctx, inside_interval_mask);

    auto outputs = zeros_like(ctx, inputs);
    float constant = std::log(std::exp(1 - min_derivative) - 1);

    unnormalized_derivatives = pad_3d(ctx, unnormalized_derivatives, {0, 0, 0, 0, 1, 1});
    unnormalized_derivatives = index_put_last_dim(ctx, unnormalized_derivatives, 0, constant);
    unnormalized_derivatives = index_put_last_dim(ctx, unnormalized_derivatives, -1, constant);

    outputs = tensor_masked_set(ctx, outputs, outside_interval_mask, tensor_masked_get(ctx, inputs, inside_interval_mask));

    auto reshaped_inputs = tensor_masked_get(ctx, inputs, inside_interval_mask);
    inputs = ggml_reshape_1d(ctx, reshaped_inputs, reshaped_inputs->ne[0]);

    auto result = rational_quadratic_spline(
            ctx,
            inputs,
            tensor_masked_get(ctx, unnormalized_widths, inside_interval_mask),
            tensor_masked_get(ctx, unnormalized_heights, inside_interval_mask),
            tensor_masked_get(ctx, unnormalized_derivatives, inside_interval_mask),
            reverse,
            tail_bound,
            min_bin_width,
            min_bin_height,
            min_derivative
    );
    outputs = tensor_masked_set(ctx, outputs, inside_interval_mask, result);
    return outputs;
}

struct ggml_tensor* vits_model::conv_flow_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning, bool reverse) {
    ASSERT(reverse, "Non reverse not supported");
    printf("Building conv flow\n");
    auto filter_channels = this->load_number("hidden_size");
    auto half_channels = (int) (this->load_number("depth_separable_channels") / 2);
    auto num_bins = this->load_number("duration_predictor_flow_bins");
    auto tail_bound = this->load_number("duration_predictor_tail_bound");
    auto [first_half, second_half] = split_3d(ctx, inputs, half_channels, half_channels, 1);
    SHAPE(first_half);
    SHAPE(second_half);

    auto hidden_states = conv1d_with_bias(ctx, first_half, this->model->get("conv_pre.weight"), this->model->get("conv_pre.bias"));
    {
        auto _0 = model->use("conv_dds");
        hidden_states = this->dilated_depth_separable_conv_graph(ctx, hidden_states, global_conditioning);
    }
    hidden_states = conv1d_with_bias(ctx, hidden_states, this->model->get("conv_proj.weight"), this->model->get("conv_proj.bias"));

    hidden_states = ggml_reshape_3d(ctx, hidden_states, first_half->ne[0], hidden_states->ne[1], first_half->ne[2]);
    hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);

    auto scale = ggml_new_f32(ctx, 1.0 / sqrt(filter_channels));
    auto unnormalized_widths = ggml_scale(ctx,
                                          slice_3d(ctx, hidden_states, 0, num_bins, 0, -1, 0, -1)
                                          , scale);
    auto unnormalized_heights = ggml_scale(ctx,
                                           slice_3d(ctx, hidden_states, num_bins, num_bins * 2, 0, -1, 0, -1),
                                           scale);
    auto unnormalized_derivatives = slice_3d(ctx, hidden_states, num_bins * 2, -1, 0, -1, 0, -1);
    SHAPE(unnormalized_widths);
    SHAPE(unnormalized_heights);
    SHAPE(unnormalized_derivatives);

    auto final_second_half = unconstrained_rational_quadratic_spline(
            ctx,
            second_half,
            unnormalized_widths,
            unnormalized_heights,
            unnormalized_derivatives,
            reverse,
            tail_bound
    );

    auto outputs = concat_3d(ctx, first_half, final_second_half, 1);
    return outputs;
}

struct ggml_tensor* vits_model::elementwise_affine_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning, bool reverse) {
    ASSERT(reverse, "Non reverse not supported");
    printf("Building elementwise affine\n");
    inputs = ggml_reshape_2d(ctx, inputs, inputs->ne[0], inputs->ne[1]);
    inputs = ggml_permute(ctx, inputs, 1, 0, 2, 3);
    inputs = ggml_cont(ctx, inputs);

    auto translation = this->model->get("translate");
    translation = ggml_permute(ctx, translation, 1, 0, 2, 3);
    translation = ggml_cont(ctx, translation);
    auto translated = ggml_add(ctx, inputs, ggml_neg(ctx, translation));

    auto log_scale = this->model->get("log_scale");
    log_scale = ggml_cont(ctx, log_scale);
    log_scale = ggml_permute(ctx, log_scale, 1, 0, 2, 3);

    auto result = ggml_mul(ctx, translated, ggml_exp(ctx, log_scale));
    result = ggml_permute(ctx, result, 1, 0, 2, 3);
    result = ggml_cont(ctx, result);
    result = ggml_reshape_3d(ctx, result, result->ne[0], result->ne[1], 1);
    return result;
}

struct ggml_tensor* vits_model::stochastic_duration_predictor_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning, bool reverse, float noise_scale_duration) {
    ASSERT(reverse, "Non reverse not supported");
    printf("Building stochastic duration predictor\n");
    auto duration_predictor_num_flows = this->load_number("duration_predictor_num_flows");
    auto _ = this->model->use("duration_predictor");
    struct ggml_tensor* log_duration = nullptr;

    inputs = conv1d_with_bias(ctx, inputs, this->model->get("conv_pre.weight"), this->model->get("conv_pre.bias"));
    if (global_conditioning != nullptr)
        ASSERT(false, "TODO global_conditioning");

    {
        auto _0 = model->use("conv_dds");
        inputs = this->dilated_depth_separable_conv_graph(ctx, inputs, global_conditioning);
    }

    inputs = conv1d_with_bias(ctx, inputs, this->model->get("conv_proj.weight"), this->model->get("conv_proj.bias"));
    if (!reverse) {
        ASSERT(reverse, "Non reverse not supported");
    } else {
        auto latents = randn(ctx, {inputs->ne[0], 2, inputs->ne[2]});
        latents = ggml_scale(ctx, latents, ggml_new_f32(ctx, noise_scale_duration));
        for(int i = duration_predictor_num_flows-1; i > -1; --i) {
            if (i == 1) continue;

            latents = flip_3d(ctx, latents, 1);
            auto _0 = model->use("flows." + std::to_string(i));
            if (i == 0) {
                latents = this->elementwise_affine_graph(ctx, latents, global_conditioning, reverse);
            } else {
                latents = this->conv_flow_graph(ctx, latents, global_conditioning, reverse);
            }
        }
        auto [log_duration_tensor, _] = split_3d(ctx, latents, 1, 1, 1);
        log_duration = log_duration_tensor;
    }
    return log_duration;
}


struct ggml_cgraph* vits_model::build_graph_part_one(struct ggml_tensor * input_ids, struct ggml_tensor* speaker_embeddings) {
    auto config = this->model->config;
    auto noise_scale_duration = this->load_float("noise_scale_duration");
    auto noise_scale = this->load_float("noise_scale");

    auto [text_encoder_output, prior_means, prior_log_variances] = this->text_encoder_graph(input_ids);
    ASSERT_SHAPE(text_encoder_output, 192, input_ids->ne[0], 1, 0);
    ASSERT_SHAPE(prior_means, 192, input_ids->ne[0], 1, 0);
    ASSERT_SHAPE(prior_log_variances, 192, input_ids->ne[0], 1, 0);
    this->text_encoder_output = text_encoder_output;
    this->prior_means_output = prior_means;
    this->prior_log_variances_output = prior_log_variances;

    auto hidden_states = text_encoder_output;
    hidden_states = ggml_permute(ctx, hidden_states, 1, 0, 2, 3);

    ASSERT(config["use_stochastic_duration_prediction"] == "True", "Only stochastic duration prediction is supported");
    auto log_duration = this->stochastic_duration_predictor_graph(ctx, hidden_states, speaker_embeddings, true, noise_scale_duration);
    auto length_scale = ggml_new_f32(ctx, 1.0 / this->speaking_rate);
    auto duration = ggml_ceil(ctx, ggml_scale(ctx, ggml_exp(ctx, log_duration), length_scale));
    this->log_duration_output = log_duration;
    ASSERT_SHAPE(this->log_duration_output, input_ids->ne[0], 1, 1, 0);
    SHAPE(duration);
    auto predicted_lengths = ggml_clamp(ctx, ggml_sum(ctx, duration), 1, std::numeric_limits<float>::max());

    this->predicted_lengths_output = tensor_max(ctx, predicted_lengths);
    this->cum_duration_output = cumsum(ctx, duration);

    struct ggml_cgraph* gf = ggml_new_graph(ctx);
    ggml_build_forward_expand(gf, this->text_encoder_output);
    ggml_build_forward_expand(gf, this->cum_duration_output);
    ggml_build_forward_expand(gf, log_duration_output);
    ggml_build_forward_expand(gf, this->prior_means_output);
    ggml_build_forward_expand(gf, this->prior_log_variances_output);
    ggml_build_forward_expand(gf, this->predicted_lengths_output);

    printf("Finished building graph\n");

    return gf;
}

struct ggml_cgraph* vits_model::build_graph_part_two(struct ggml_tensor* input_ids, struct ggml_tensor * cum_duration, struct ggml_tensor* prior_means, struct ggml_tensor* prior_log_variances, struct ggml_tensor* speaker_embeddings, int predicted_length) {
    printf("Building graph part two, output_length %d\n", predicted_length);
    auto config = this->model->config;
    auto noise_scale_duration = this->load_float("noise_scale_duration");
    auto noise_scale = this->load_float("noise_scale");

    auto indices = tensor_arange(ctx, predicted_length);
    indices = ggml_reshape_2d(ctx, indices, indices->ne[0], 1);
    SHAPE(indices);

    cum_duration = ggml_reshape_2d(ctx, cum_duration, 1, cum_duration->ne[0]);
    SHAPE(cum_duration);


    struct ggml_tensor* valid_indices = nullptr;
    for (int i = 0; i < indices->ne[0]; ++i) {
        auto indices_i = slice_2d(ctx, indices, i, i+1, 0, -1);
        indices_i = ggml_repeat(ctx, indices_i, cum_duration);
        auto result_i = tensor_compare(ctx, indices_i, cum_duration, [](float a, float b) { return a < b; });
        SHAPE(result_i)
        if (valid_indices == nullptr) {
            valid_indices = result_i;
        } else {
            valid_indices = concat_2d(ctx, valid_indices, result_i, 0);
        }
    }
    SHAPE(valid_indices);
    valid_indices = ggml_reshape_3d(ctx, valid_indices, valid_indices->ne[0], valid_indices->ne[1], 1);
    auto padded_valid_indices = pad_3d(ctx, valid_indices, {0, 0, 1, 0, 0, 0});
    SHAPE(padded_valid_indices);
    auto minus = slice_3d(ctx, padded_valid_indices, 0, -1, 0, -2, 0, -1);
    SHAPE(minus);
    auto padded_indices = ggml_sub(ctx, valid_indices, minus);
    SHAPE(padded_indices);
    struct ggml_tensor* attn = ggml_permute(ctx, padded_indices, 1, 0, 2, 3);
    attn = ggml_cont(ctx, attn);
    attn = ggml_reshape_2d(ctx, attn, attn->ne[0], attn->ne[1]);

    prior_means = ggml_permute(ctx, prior_means, 1, 0, 2, 3);
    prior_means = ggml_cont(ctx, prior_means);
    prior_means = ggml_reshape_2d(ctx, prior_means, prior_means->ne[0], prior_means->ne[1]);

    prior_log_variances = ggml_permute(ctx, prior_log_variances, 1, 0, 2, 3);
    prior_log_variances = ggml_cont(ctx, prior_log_variances);
    prior_log_variances = ggml_reshape_2d(ctx, prior_log_variances, prior_log_variances->ne[0], prior_log_variances->ne[1]);


    prior_means = ggml_mul_mat(ctx, prior_means, attn);
    prior_means = ggml_permute(ctx, prior_means, 1, 0, 2, 3);
    prior_means = ggml_cont(ctx, prior_means);
    SHAPE(prior_means);

    prior_log_variances = ggml_mul_mat(ctx, prior_log_variances, attn);
    prior_log_variances = ggml_permute(ctx, prior_log_variances, 1, 0, 2, 3);
    prior_log_variances = ggml_cont(ctx, prior_log_variances);
    SHAPE(prior_log_variances);

    auto noise = randn_like(ctx, prior_means);
    noise = ggml_mul(ctx, noise, ggml_exp(ctx, prior_log_variances));
    noise = ggml_scale(ctx, noise, ggml_new_f32(ctx, noise_scale));
    SHAPE(noise);

    auto prior_latents = ggml_add(ctx, prior_means, noise);
    prior_latents = ggml_reshape_3d(ctx, prior_latents, prior_latents->ne[0], prior_latents->ne[1], 1);
    auto latents = this->flow_graph(ctx, prior_latents, speaker_embeddings, true);
    this->latents_output = latents;
    ASSERT_SHAPE(this->latents_output, 93, 192, 1, 0);

    this->waveform = this->hifigan_graph(ctx, latents, speaker_embeddings);
    SHAPE(this->waveform)
    ASSERT_SHAPE(this->waveform, 23808, 1, 1, 1);

    auto gf = ggml_build_forward_ctx(ctx, this->waveform);

    printf("Finished building graph\n");

    return gf;
}

void vits_model::execute_graph(struct ggml_context* ctx, struct ggml_cgraph* graph) {
    printf("Allocating memory for work computation graph...\n");
    int threads = std::min((int)std::thread::hardware_concurrency(), 2);
    auto plan = ggml_graph_plan(graph, threads);
    if (plan.work_size > 0) {
        plan.work_data = (uint8_t*) malloc(plan.work_size);
    }

    printf("Computing...\n");
    auto start = std::chrono::high_resolution_clock::now();
    ggml_graph_compute(graph, &plan);
    auto end = std::chrono::high_resolution_clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    free(plan.work_data);
    printf("Computation took %lld milliseconds\n", delta);
}

std::vector<float> vits_model::process(std::string phonemes) {
    // tokenize phonemes TODO
    auto debug_mode = true;
    std::vector<int32_t> input_ids = {0, 19,  0, 39,  0, 35,  0, 35,  0, 41,  0, 27,  0, 41,  0, 43,  0, 35, 0, 29,  0};
    auto input_ids_tensor = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, input_ids.size());
    memcpy(input_ids_tensor->data, input_ids.data(), ggml_element_size(input_ids_tensor) * input_ids.size());

    struct ggml_tensor* speaker_embeddings = nullptr;
    auto graph_one = this->build_graph_part_one(input_ids_tensor, speaker_embeddings);
    printf("Executing graph one\n");
    this->execute_graph(ctx, graph_one);

    auto predicted_length = (int) ((float*) this->predicted_lengths_output->data)[0];
    printf("predicted length %d\n", predicted_length);

    auto graph_two = this->build_graph_part_two(input_ids_tensor, this->cum_duration_output, this->prior_means_output, this->prior_log_variances_output, speaker_embeddings, predicted_length);
    printf("Executing graph two\n");
    this->execute_graph(ctx, graph_two);

    if (debug_mode) {
        ASSERT_STARTS_WITH(this->text_encoder_output, 0.1938, 0.2144, 0.1059);
        ASSERT_STARTS_WITH(this->prior_means_output, 0.4238,  0.1439,  0.1764);
        ASSERT_STARTS_WITH(this->prior_log_variances_output, -0.2889, -0.0325, -0.2308);
        ASSERT_STARTS_WITH(this->latents_output, 0.9742,  2.0036,  1.5632);
        ASSERT_STARTS_WITH(this->log_duration_output, 3.1618, -0.1879,  0.7810);
        ASSERT_STARTS_WITH(this->waveform, -3.2723e-05, -1.2340e-05,  2.3337e-05);
    }

    //ASSERT(this->last_hidden_state->ne[2] == 1, "Batch size must be 1");
    //ASSERT(this->last_hidden_state->type == GGML_TYPE_F32, "Type must be float32");

    //if (last_hidden_state != nullptr)
    //    PRINT_TENSOR2(this->last_hidden_state);

    return std::vector<float>((float *) this->waveform->data, (float *) this->waveform->data + this->waveform->ne[0]);
}

vits_model * vits_model_load_from_file(const char * path) {
    struct ggml_init_params params = {
            .mem_size   = (size_t)2*1024*1024*1024, // 2gb
            .mem_buffer = nullptr,
    };

    struct ggml_context * ctx = ggml_init(params);
    printf("Initialized ggml context with %d mb\n", params.mem_size / 1024 / 1024);
    auto model_data = vits_model_data::from_file(path, ctx);
    return new vits_model(ctx, std::move(model_data), 1);
}

void vits_free_model(vits_model * model) {
    delete model;
}

void vits_free_result(vits_result result) {
    delete result.data;
}

vits_result vits_model_process(vits_model * model, const char * phonemes) {
    auto result = model->process(phonemes);
    vits_result r;
    r.data = new float[result.size()];
    r.size = result.size();
    memcpy(r.data, result.data(), result.size());
    return r;
}
