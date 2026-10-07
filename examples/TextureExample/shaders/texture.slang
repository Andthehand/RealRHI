
struct VSInput
{
    float2 pos;
    float2 uv;

    uint texIdx;
    uint samplerIdx;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv;
    uint texIdx;
    uint samplerIdx;
};

[require(spvDescriptorHeapEXT)]
[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    output.texIdx = input.texIdx;
    output.samplerIdx = input.samplerIdx;
    return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    Texture2D tex = Texture2D.Handle(uint2(input.texIdx, 0));
    SamplerState samp = SamplerState.Handle(uint2(input.samplerIdx, 0));
    return tex.Sample(samp, input.uv);
}
