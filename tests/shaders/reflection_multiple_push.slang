struct Data { float4 value; };
[[vk::push_constant]] ConstantBuffer<Data> first;
[[vk::push_constant]] ConstantBuffer<Data> second;
[shader("vertex")]
float4 vertexMain(uint id : SV_VertexID) : SV_Position { return first.value + second.value; }
