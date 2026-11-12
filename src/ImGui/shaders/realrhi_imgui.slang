struct PushData {
    float2 scale;
    float2 translate;
    uint textureIndex;
    uint samplerIndex;
};
[[vk::push_constant]] ConstantBuffer<PushData> pushData;

struct VertexInput {
    float2 position;
    float2 uv;
    float4 color;
};
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv;
    float4 color;
};

[require(spvDescriptorHeapEXT)]
[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position * pushData.scale + pushData.translate, 0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    Texture2D texture = Texture2D.Handle(uint2(pushData.textureIndex, 0));
    SamplerState sampler = SamplerState.Handle(uint2(pushData.samplerIndex, 0));
    return input.color * texture.Sample(sampler, input.uv);
}
