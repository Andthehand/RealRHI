struct TextureParameters {
	float4 a;
	float4 b;
	Sampler2D myTexture;
};

ParameterBlock<TextureParameters> parameters1;
ParameterBlock<TextureParameters> parameters2;

struct VSInput {
	float2 pos;
	float2 texCoord;
};

struct VSOutput {
	float4 pos : SV_Position;
	float2 texCoord;
};

[shader("vertex")]
VSOutput main(VSInput input) {
	VSOutput output;

	output.pos = float4(input.pos, 0.0, 1.0);
	output.texCoord = input.texCoord;

	return output;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
	return parameters1.myTexture.Sample(input.texCoord);
}
