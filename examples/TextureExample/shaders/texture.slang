
struct VSInput {
	float2 pos;
	float2 uv;

	uint textureIndex;
	uint samplerIndex;
};

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv;
	uint textureIndex;
	uint samplerIndex;
};

[require(spvDescriptorHeapEXT)]
[shader("vertex")]
VSOutput vertexMain(VSInput input) {
	VSOutput output;
	output.pos = float4(input.pos, 0.0, 1.0);
	output.uv = input.uv;
	output.textureIndex = input.textureIndex;
	output.samplerIndex = input.samplerIndex;
	return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
	Texture2D texture = Texture2D.Handle(uint2(input.textureIndex, 0));
	SamplerState sampler = SamplerState.Handle(uint2(input.samplerIndex, 0));
	return texture.Sample(sampler, input.uv);
}
