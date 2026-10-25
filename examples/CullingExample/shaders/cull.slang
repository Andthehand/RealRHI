struct CullParameters {
	uint output;
	float viewHalfWidth;
};

[[vk::push_constant]]
ConstantBuffer<CullParameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(4, 1, 1)]
void cullMain(uint3 id : SV_DispatchThreadID) {
	const float centers[4] = {-0.5, 0.5, -2.0, 2.0};
	if (id.x >= 4) {
		return;
	}

	// Conservative interval/frustum test against a triangle's horizontal bounds.
	uint visible = abs(centers[id.x]) <= parameters.viewHalfWidth + 0.2 ? 1 : 0;
	RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(parameters.output, 0));
	uint base = 4 + id.x * 12;
	output[base + 0] = 3;
	output[base + 1] = visible;
	output[base + 2] = id.x * 3;
	output[base + 3] = 0;

	output[base + 4] = 3;
	output[base + 5] = visible;
	output[base + 6] = id.x * 3;
	output[base + 7] = 0;
	output[base + 8] = 0;
	output[base + 9] = 0;
	output[base + 10] = 0;
	output[base + 11] = 0;
}
