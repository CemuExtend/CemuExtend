#version 450

layout(location = 0) out vec2 uv;

void main()
{
	const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	// Vulkan maps NDC y=-1 to the top of a positive-height viewport. CEF's
	// first BGRA row is also the top row, so v=0 belongs at that vertex.
	const vec2 coordinates[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
	uv = coordinates[gl_VertexIndex];
}
