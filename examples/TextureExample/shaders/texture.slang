
struct VSInput
{
    float2 pos;
    float2 uv;

    uint texIdx;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv;

    uint texIdx;
};

[require(spvDescriptorHeapEXT)]
[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    output.texIdx = input.texIdx;
    return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    Texture2D tex = Texture2D.Handle(uint2(input.texIdx, 0));
    SamplerState samp = SamplerState.Handle(uint2(0, 0));
    return tex.Sample(samp, input.uv);
}
