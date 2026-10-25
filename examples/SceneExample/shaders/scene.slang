import scene_common;

struct VertexOutput {
	float4 position : SV_Position;
	float2 uv;
	float diffuse;
	float4 lightPosition;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
	float3 world = WorldPosition(input.position);
	float cameraY = 0.94 * world.y - 0.342 * world.z;
	float distance = 7.0 - (0.342 * world.y + 0.94 * world.z);

	VertexOutput output;
	// Vulkan uses a downward-facing viewport and a zero-to-one depth range.
	output.position =
		float4(world.x * 1.8 / parameters.aspectRatio, -cameraY * 1.8, (100.0 * distance - 10.0) / 99.9, distance);
	output.uv = input.uv;
	output.diffuse = saturate(dot(Rotate(input.normal), LightDirection()));
	output.lightPosition = LightPosition(world);
	return output;
}

[require(spvDescriptorHeapEXT)]
float ShadowVisibility(float4 lightPosition) {
	if (parameters.shadowsEnabled == 0) {
		return 1.0;
	}

	float3 projected = lightPosition.xyz / lightPosition.w;
	float2 uv = projected.xy * 0.5 + 0.5;
	if (any(uv < 0) || any(uv > 1) || projected.z < 0 || projected.z > 1) {
		return 1.0;
	}

	Texture2D<float> shadow = Texture2D<float>.Handle(uint2(parameters.shadowTextureIndex, 0));
	SamplerComparisonState sampler = SamplerComparisonState.Handle(uint2(parameters.shadowSamplerIndex, 0));
	float visibility = 0.0;

	// Average comparison results rather than filtering depth values across an occluder's edge.
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			visibility += shadow.SampleCmpLevelZero(
				sampler, uv + float2(x, y) * parameters.shadowTexelSize, projected.z - 0.0005);
		}
	}
	return visibility / 9.0;
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
	Texture2D<float4> pattern = Texture2D<float4>.Handle(uint2(parameters.textureIndex, 0));
	SamplerState sampler = SamplerState.Handle(uint2(parameters.samplerIndex, 0));
	float visibility = ShadowVisibility(input.lightPosition);
	float lighting = 0.3 + 0.7 * input.diffuse * visibility;
	float3 color = pattern.Sample(sampler, input.uv).rgb * parameters.tint.rgb * lighting;
	return float4(color, parameters.tint.a);
}
