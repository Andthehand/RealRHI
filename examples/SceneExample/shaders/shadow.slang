import scene_common;

[shader("vertex")]
float4 vertexMain(float3 position) : SV_Position {
	return LightPosition(WorldPosition(position));
}
