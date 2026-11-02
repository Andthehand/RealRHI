struct Parameters {
	uint imageIndex;
	uint width;
	uint height;
	uint outputIndex;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(8, 8, 1)]
void renderMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= parameters.width || id.y >= parameters.height) {
		return;
	}
	RWTexture2D<float4> image = RWTexture2D<float4>.Handle(uint2(parameters.imageIndex, 0));
	// UNorm storage writes do not perform sRGB encoding. These are display-encoded values.
	image[id.xy] = float4(float(id.x) / max(parameters.width - 1, 1u),
		float(id.y) / max(parameters.height - 1, 1u), 0.25, 1.0);
}

// Used by the integration test to verify the actual presented image's contents.
[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(8, 8, 1)]
void readMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= parameters.width || id.y >= parameters.height) {
		return;
	}
	RWTexture2D<float4> image = RWTexture2D<float4>.Handle(uint2(parameters.imageIndex, 0));
	RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(parameters.outputIndex, 0));
	uint4 color = uint4(round(saturate(image[id.xy]) * 255.0));
	output[id.y * parameters.width + id.x] = color.x | (color.y << 8) | (color.z << 16) | (color.w << 24);
}
