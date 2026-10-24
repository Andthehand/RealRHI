struct Parameters {
	float4 color;
	float4 depth;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[shader("vertex")]
float4 vertexMain(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID) : SV_Position {
	float2 positions[] = {float2(-0.4, -0.8), float2(0.4, -0.8), float2(0, 0.8)};
	
	float2 position = positions[vertexId - 5];
	position.x += -0.5 + float(instanceId - 7);

	return float4(position, parameters.depth.x, 1);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target {
	return parameters.color;
}
