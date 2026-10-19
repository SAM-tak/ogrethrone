#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D scene;
vulkan( layout( ogre_s0 ) uniform sampler sceneSampler );
vulkan( layout( ogre_P0 ) uniform Params { )
    uniform vec4 invTex0Size;
    uniform float threshold;
    uniform float knee;
vulkan( }; )

vulkan_layout( location = 0 )
in block { vec2 uv0; } inPs;
vulkan_layout( location = 0 )
out vec4 fragColour;

vec3 bright( vec2 uv )
{
    vec3 colour = max( texture( vkSampler2D( scene, sceneSampler ), uv ).rgb, vec3( 0.0 ) );
    float brightness = max( colour.r, max( colour.g, colour.b ) );
    // A soft knee avoids an abrupt glow transition as a highlight crosses the threshold.
    float soft = clamp( brightness - threshold + knee, 0.0, 2.0 * knee );
    soft = soft * soft / (4.0 * knee + 0.00001);
    return colour * max( brightness - threshold, soft ) / max( brightness, 0.00001 );
}

void main()
{
    // Four bilinear samples cover the 4x4 source footprint before quarter-size blur.
    vec2 stepSize = invTex0Size.xy;
    vec3 colour = bright( inPs.uv0 + stepSize * vec2( -1, -1 ) );
    colour += bright( inPs.uv0 + stepSize * vec2( 1, -1 ) );
    colour += bright( inPs.uv0 + stepSize * vec2( -1, 1 ) );
    colour += bright( inPs.uv0 + stepSize * vec2( 1, 1 ) );
    fragColour = vec4( colour * 0.25, 1.0 );
}
