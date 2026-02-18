struct VSInput {
    float2 Pos;
    float3 Color;
};

struct VSOutput {
    float4 Pos   : SV_Position;
    float3 Color;
};

[shader("vertex")]
VSOutput main(VSInput input) {
    VSOutput output;

    output.Pos = float4(input.Pos, 0.0, 1.0);
    output.Color = input.Color;

    return output;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    return float4(input.Color, 1.0);
}
