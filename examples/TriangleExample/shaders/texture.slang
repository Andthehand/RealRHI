struct test {
    float4 a;
    float4 b;
    Sampler2D myTexture;
};

ParameterBlock<test> tes1;
ParameterBlock<test> tes2;

struct VSInput {
    float2 Pos;
    float2 TexCoord;
};

struct VSOutput {
    float4 Pos   : SV_Position;
    float2 TexCoord;
};

[shader("vertex")]
VSOutput main(VSInput input) {
    VSOutput output;

    output.Pos = float4(input.Pos, 0.0, 1.0);
    output.TexCoord = input.TexCoord;

    return output;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    return tes1.myTexture.Sample(input.TexCoord);
}