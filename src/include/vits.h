
#ifndef VITS_H
#define VITS_H

#include <ggml/ggml.h>
#include <stdlib.h>
#include <string>
#include <stdint.h>
#include <vector>
#include <sstream>
#include "vits_model_data.h"

typedef struct ggml_tensor * tensor_t;

class vits_model {
private:
    int speaking_rate;
    std::unique_ptr<vits_model_data> model;
    struct ggml_context * ctx;
    struct ggml_tensor * last_hidden_state;
    struct ggml_tensor * waveform;
    struct ggml_tensor * cum_duration_output;
    struct ggml_tensor * predicted_lengths_output;
    struct ggml_tensor * text_encoder_output;
    struct ggml_tensor * prior_means_output;
    struct ggml_tensor * prior_log_variances_output;
    struct ggml_tensor * log_duration_output;
    struct ggml_tensor * latents_output;
    int load_number(std::string key);
    float load_float(std::string key);
    std::string load_param(std::string key);
    template<typename T>
    std::vector<T> load_vector(const std::string& key) {
        printf("Loading vector %s\n", key.c_str());
        std::string serialized_data = this->load_param(key); // Assuming this->load_param is defined somewhere in your code
        return load_vector_impl<T>(serialized_data);
    };
    template<typename T>
    std::vector<T> load_vector_impl(const std::string& serialized_data);


public:
    vits_model(struct ggml_context* ctx, std::unique_ptr<vits_model_data> model, int speaking_rate);
    ~vits_model();
    void execute_graph(struct ggml_context* ctx, struct ggml_cgraph* graph);
    struct ggml_cgraph* build_graph_part_one(struct ggml_tensor * input_ids, struct ggml_tensor* speaker_embeddings);
    struct ggml_cgraph* build_graph_part_two(struct ggml_tensor* input_ids, struct ggml_tensor * cum_duration, struct ggml_tensor* prior_means, struct ggml_tensor* prior_log_variances, struct ggml_tensor* speaker_embeddings, int output_length);

    struct std::tuple<ggml_tensor*, ggml_tensor*, ggml_tensor*> text_encoder_graph(struct ggml_tensor* input_ids);
    struct ggml_tensor* wavenet_graph(struct ggml_tensor* input, struct ggml_tensor* speaker_embedding);
    struct ggml_tensor* flow_graph(struct ggml_context* ctx, struct ggml_tensor* inputs, struct ggml_tensor* conditioning, bool reverse);
    struct ggml_tensor* hifigan_graph(struct ggml_context* ctx, struct ggml_tensor * input_ids, struct ggml_tensor* global_conditioning);
    struct ggml_tensor* dilated_depth_separable_conv_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning);
    struct ggml_tensor* elementwise_affine_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning, bool reverse);
    struct ggml_tensor* conv_flow_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* global_conditioning, bool reverse);
    struct ggml_tensor* stochastic_duration_predictor_graph(struct ggml_context* ctx, struct ggml_tensor * inputs, struct ggml_tensor* speaker_embeddings, bool reverse, float noise_scale_duration);
    struct ggml_tensor* hifigan_residual_block_graph(struct ggml_context *ctx, struct ggml_tensor *hidden_states, int kernel_size, std::vector<int> dilation, double leaky_relu_slope);
    std::vector<float> process(std::string phonemes);
};

#define VITS_API extern "C" __attribute__((visibility("default")))

typedef struct vits_result {
    float * data;
    size_t size;
} vits_result;

VITS_API vits_model * vits_model_load_from_file(const char * path);

VITS_API void vits_free_model(vits_model * model);

VITS_API void vits_free_result(vits_result result);

VITS_API vits_result vits_model_process(vits_model * model, const char * phonemes);

#endif