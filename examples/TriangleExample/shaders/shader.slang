struct VSInput {
	float2 pos;
	float3 color;
};

struct VSOutput {
	float4 pos : SV_Position;
	float3 color;
};

[shader("vertex")]
VSOutput main(VSInput input) {
	VSOutput output;

	output.pos = float4(input.pos, 0.0, 1.0);
	output.color = input.color;

	return output;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
	return float4(input.color, 1.0);
}
