[shader("vertex")]
float4 vertexMain(uint vertex : SV_VulkanVertexID) : SV_Position {
	const float centers[4] = {-0.5, 0.5, -2.0, 2.0};
	const float2 corners[3] = {float2(-0.2, -0.5), float2(0.2, -0.5), float2(0.0, 0.5)};
	return float4(corners[vertex % 3] + float2(centers[vertex / 3], 0), 0, 1);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target0 {
	return float4(0, 1, 0, 1);
}
