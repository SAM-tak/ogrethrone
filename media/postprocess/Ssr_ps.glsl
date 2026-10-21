#version ogre_glsl_ver_330


// Screen-space reflections from the depth prepass: marches the view's reflection off each surface
// through the depth buffer, and where it meets a surface takes that surface's colour from the
// previous frame (reprojected by the motion vectors). Alpha is the confidence, which HlmsPbs
// uses to blend the reflection over the reflection probes: it fades out on rough surfaces, near
// the screen's edges and for rays towards the camera, which the screen cannot show.

vulkan_layout( ogre_t0 ) uniform texture2D normals;
vulkan( layout( ogre_s0 ) uniform sampler normalSampler );
vulkan_layout( ogre_t1 ) uniform texture2D shadowRoughness;
vulkan( layout( ogre_s1 ) uniform sampler shadowRoughnessSampler );
vulkan_layout( ogre_t2 ) uniform texture2D depth;
vulkan( layout( ogre_s2 ) uniform sampler depthSampler );
vulkan_layout( ogre_t3 ) uniform texture2D motion;
vulkan( layout( ogre_s3 ) uniform sampler motionSampler );
vulkan_layout( ogre_t4 ) uniform texture2D previousFrame;
vulkan( layout( ogre_s4 ) uniform sampler previousSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The view's projection (into clip space with UV y as the targets store it) and its inverse.
	uniform mat4 projection;
	uniform mat4 inverseProjection;
	// The change of the projection's jitter since the previous frame, in UV.
	uniform vec2 jitterDelta;
	// How far reflections are traced, in metres, and how thick a surface counts as.
	uniform float maxDistance;
	uniform float thickness;
	// 0 turns the reflections off (also while the previous frame is unusable).
	uniform float strength;
	// 1 when the depth buffer is reversed (the far plane is 0).
	uniform float reverseDepth;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 reflection;

const int steps = 32;
const int refinements = 4;

vec3 viewPosition( vec2 uv, float d )
{
	vec4 position = inverseProjection * vec4( uv * 2.0 - 1.0, d, 1.0 );
	return position.xyz / position.w;
}

vec2 project( vec3 position )
{
	vec4 clip = projection * vec4( position, 1.0 );
	return clip.xy / clip.w * 0.5 + 0.5;
}

/// The view-space depth of the surface drawn at @p uv, and whether there is one.
float surfaceAt( vec2 uv, ivec2 size, out bool drawn )
{
	ivec2 texel = clamp( ivec2( uv * vec2( size ) ), ivec2( 0 ), size - 1 );
	float d = texelFetch( vkSampler2D( depth, depthSampler ), texel, 0 ).x;
	drawn = d != ( reverseDepth > 0.5 ? 0.0 : 1.0 );
	return viewPosition( uv, d ).z;
}

void main()
{
	reflection = vec4( 0.0 );
	ivec2 size = textureSize( vkSampler2D( depth, depthSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	float d = texelFetch( vkSampler2D( depth, depthSampler ), pixel, 0 ).x;
	if( strength <= 0.0 || d == ( reverseDepth > 0.5 ? 0.0 : 1.0 ) )
		return;
	float roughness = texelFetch( vkSampler2D( shadowRoughness, shadowRoughnessSampler ), pixel, 0 ).y * 0.98 + 0.02;
	float glossy = 1.0 - smoothstep( 0.25, 0.55, roughness );
	if( glossy <= 0.0 )
		return;

	vec2 uv = ( vec2( pixel ) + 0.5 ) / vec2( size );
	vec3 position = viewPosition( uv, d );
	vec3 normal = normalize( texelFetch( vkSampler2D( normals, normalSampler ), pixel, 0 ).xyz * 2.0 - 1.0 );
	vec3 ray = reflect( normalize( position ), normal );
	// The view looks along -z; rays towards the camera leave the screen's reach.
	float away = 1.0 - smoothstep( -0.1, 0.3, ray.z );
	if( away <= 0.0 )
		return;

	// March with steps that grow with distance, offset per pixel by noise.
	float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) );
	float previousT = 0.0;
	bool hit = false;
	float t = 0.0;
	for( int i = 1; i <= steps && !hit; ++i )
	{
		float s = ( float( i ) - 1.0 + noise ) / float( steps );
		t = maxDistance * s * s + 0.02;
		vec3 point = position + ray * t;
		vec2 at = project( point );
		if( any( lessThan( at, vec2( 0.0 ) ) ) || any( greaterThan( at, vec2( 1.0 ) ) ) || point.z > -0.01 )
			break;
		bool drawn;
		float surface = surfaceAt( at, size, drawn );
		float behind = surface - point.z;
		if( drawn && behind > 0.0 && behind < thickness + t * 0.05 )
			hit = true;
		else
			previousT = t;
	}
	if( !hit )
		return;
	// Close in on the surface.
	float low = previousT, high = t;
	for( int i = 0; i < refinements; ++i )
	{
		float middle = ( low + high ) * 0.5;
		vec3 point = position + ray * middle;
		bool drawn;
		if( surfaceAt( project( point ), size, drawn ) > point.z )
			high = middle;
		else
			low = middle;
	}
	vec2 hitUv = project( position + ray * high );

	// Where that surface was in the previous frame, without the jitter.
	ivec2 hitTexel = clamp( ivec2( hitUv * vec2( size ) ), ivec2( 0 ), size - 1 );
	vec2 previousUv = hitUv - ( texelFetch( vkSampler2D( motion, motionSampler ), hitTexel, 0 ).xy - jitterDelta );
	vec2 edge = min( previousUv, 1.0 - previousUv );
	float inside = smoothstep( 0.0, 0.08, min( edge.x, edge.y ) );
	vec3 colour = texture( vkSampler2D( previousFrame, previousSampler ), previousUv ).rgb;
	reflection = vec4( colour, strength * glossy * away * inside * ( 1.0 - smoothstep( 0.6, 1.0, high / maxDistance ) ) );
}
