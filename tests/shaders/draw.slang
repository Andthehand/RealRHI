struct Parameters {
	float4 color;
	float4 depth;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexInput {
	float2 position;
};

[shader("vertex")]
float4 vertexMain(VertexInput input) : SV_Position {
	return float4(input.position, parameters.depth.x, 1);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target {
	return parameters.color;
}
