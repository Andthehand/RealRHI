struct VertexOutput {
	float4 position : SV_Position;
	float3 color;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
	const float2 positions[3] = {float2(0, -0.65), float2(0.65, 0.65), float2(-0.65, 0.65)};
	const float3 colors[3] = {float3(1, 0.15, 0.1), float3(0.1, 1, 0.25), float3(0.15, 0.3, 1)};
	VertexOutput output;
	output.position = float4(positions[vertexId], 0, 1);
	output.color = colors[vertexId];
	return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
	return float4(input.color, 1);
}
