struct Parameters {
	uint textureIndex;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(4, 4, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
	RWTexture2D<float4> image = RWTexture2D<float4>.Handle(uint2(parameters.textureIndex, 0));
	image[id.xy] = 1.0 - image[id.xy];
}
