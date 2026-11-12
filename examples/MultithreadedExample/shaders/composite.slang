struct Parameters {
    uint textureIndex;
    uint samplerIndex;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0, 1);
    output.uv = uv;
    return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    Texture2D<float4> tile = Texture2D<float4>.Handle(uint2(parameters.textureIndex, 0));
    SamplerState sampler = SamplerState.Handle(uint2(parameters.samplerIndex, 0));
    return tile.SampleLevel(sampler, input.uv, 0);
}
