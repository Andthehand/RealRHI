struct Parameters {
	float4 translationAndScale;
	float4 tint;
	float aspectRatio;
	float rotationSine;
	float rotationCosine;
	float padding;
	uint textureIndex;
	uint samplerIndex;
	uint shadowTextureIndex;
	uint shadowSamplerIndex;
	float shadowTexelSize;
	uint shadowsEnabled;
	float2 shadowPadding;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexInput {
	float3 position;
	float2 uv;
	float3 normal;
};

float3 Rotate(float3 value) {
	float sine = parameters.rotationSine;
	float cosine = parameters.rotationCosine;
	return float3(value.x * cosine + value.z * sine, value.y, -value.x * sine + value.z * cosine);
}

float3 WorldPosition(float3 position) {
	return Rotate(position * parameters.translationAndScale.w) + parameters.translationAndScale.xyz;
}

float3 LightDirection() {
	return normalize(float3(0.4, 0.8, -0.6));
}

float4 LightPosition(float3 world) {
	float3 direction = LightDirection();
	float3 right = normalize(cross(float3(0, 1, 0), direction));
	float3 up = cross(direction, right);

	// Orthographic light camera covers the entire floor, with Vulkan's downward Y and zero-to-one depth.
	return float4(dot(world, right) / 5.0, -dot(world, up) / 5.0, (8.0 - dot(world, direction)) / 16.0, 1.0);
}
