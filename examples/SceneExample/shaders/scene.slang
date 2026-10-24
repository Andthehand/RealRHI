struct Parameters {
	float4 translationAndScale;
	float4 tint;
	float aspectRatio;
	float rotationSine;
	float rotationCosine;
	float padding;
	uint textureIndex;
	uint samplerIndex;
	float2 descriptorPadding;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexInput {
	float3 position;
	float2 uv;
	float3 normal;
};

struct VertexOutput {
	float4 position : SV_Position;
	float2 uv;
	float lighting;
};

float3 Rotate(float3 value) {
	float sine = parameters.rotationSine;
	float cosine = parameters.rotationCosine;
	return float3(value.x * cosine + value.z * sine, value.y, -value.x * sine + value.z * cosine);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
	float3 world = Rotate(input.position * parameters.translationAndScale.w) + parameters.translationAndScale.xyz;
	float cameraY = 0.94 * world.y - 0.342 * world.z;
	float distance = 7.0 - (0.342 * world.y + 0.94 * world.z);

	VertexOutput output;
	// Vulkan uses a downward-facing viewport and a zero-to-one depth range.
	output.position = float4(world.x * 1.8 / parameters.aspectRatio, -cameraY * 1.8,
		(100.0 * distance - 10.0) / 99.9, distance);
	output.uv = input.uv;
	output.lighting = 0.3 + 0.7 * saturate(dot(Rotate(input.normal), normalize(float3(0.4, 0.8, 1.0))));
	return output;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
	Texture2D<float4> pattern = Texture2D<float4>.Handle(uint2(parameters.textureIndex, 0));
	SamplerState sampler = SamplerState.Handle(uint2(parameters.samplerIndex, 0));
	float3 color = pattern.Sample(sampler, input.uv).rgb * parameters.tint.rgb * input.lighting;
	return float4(color, parameters.tint.a);
}
