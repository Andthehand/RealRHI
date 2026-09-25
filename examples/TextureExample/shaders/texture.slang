
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
    return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 main(VSOutput input) : SV_Target
{
    return material.albedo.Sample(input.uv);
}
