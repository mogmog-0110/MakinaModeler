#version 3.7;

global_settings {
	assumed_gamma 1.0
	max_trace_level 8
	radiosity {
		pretrace_start 0.08  pretrace_end 0.01
		count 150  nearest_count 8  error_bound 0.4
		recursion_limit 2  brightness 1.0
	}
}

#declare ClayFinish = finish {
	ambient 0.0  diffuse 0.72
	specular 0.18  roughness 0.09  brilliance 0.9
}
#declare ClayBump = normal { bumps 0.018  scale 0.09 }

#declare Charcoal = texture {          // 体。真っ黒でなく緑がかった炭色
	pigment { color srgb <0.055, 0.075, 0.072> }
	finish { ClayFinish }  normal { ClayBump }
}
#declare Cream = texture {             // 腹
	pigment { color srgb <0.945, 0.930, 0.895> }
	finish { ClayFinish }  normal { ClayBump }
}
#declare Amber = texture {             // 腹のふちの黄色い線
	pigment { color srgb <0.98, 0.72, 0.10> }
	finish { ClayFinish }  normal { ClayBump }
}
#declare Vermilion = texture {         // 嘴
	pigment { color srgb <0.88, 0.13, 0.13> }
	finish { ClayFinish }  normal { ClayBump }
}
#declare Tangerine = texture {         // 足
	pigment { color srgb <0.95, 0.48, 0.10> }
	finish { ClayFinish }  normal { ClayBump }
}
#declare EyeWhite = texture {
	pigment { color srgb <0.97, 0.96, 0.94> }
	finish { ambient 0.0 diffuse 0.7 specular 0.35 roughness 0.03 }
}
#declare EyeBlack = texture {
	pigment { color srgb <0.03, 0.03, 0.035> }
	finish { ambient 0.0 diffuse 0.4 specular 0.6 roughness 0.02 }
}

#declare BodyProfile = sor {
	8,
	<0.14, -0.12>,
	<0.03,  0.02>,
	<0.34,  0.14>,
	<0.508, 0.42>,   // 一番太いのは腰
	<0.472, 0.84>,
	<0.385, 1.24>,   // 肩へ向けてすぼめる
	<0.03,  1.50>,
	<0.14,  1.64>
	sturm
}

#declare Torso = object {
	BodyProfile
	scale <1, 1, 0.88>
	texture { Charcoal }
}

#declare Head = sphere {
	<0, 0, 0>, 1
	scale <0.335, 0.255, 0.315>
	rotate x*-3
	translate <0, 1.60, 0.015>
	texture { Charcoal }
}

#declare RimOutline   = object { BodyProfile  scale <0.806, 1.0, 6.0> }
#declare WhiteOutline = object { BodyProfile  scale <0.740, 1.0, 6.0> }

#declare BellyRim = intersection {
	sphere { <0, 0, 0>, 1  scale <0.52, 0.742, 0.200>  translate <0, 0.680, -0.262> }
	object { RimOutline }
	texture { Amber }
}
#declare Belly = intersection {
	sphere { <0, 0, 0>, 1  scale <0.52, 0.700, 0.222>  translate <0, 0.680, -0.288> }
	object { WhiteOutline }
	texture { Cream }
}

#declare BeakShape = blob {
	threshold 0.55
	sphere   { <0,  0.26, 0>, 1.00, 1.0  scale <1.00, 0.52, 0.52> }
	cylinder { <0, -0.12, 0>, <0, 0.06, 0>, 0.92, 0.9  scale <1.00, 0.40, 0.52> }
}
#declare Beak = object {
	BeakShape
	scale 0.255
	rotate x*6
	translate <0, 1.500, -0.246>
	texture { Vermilion }
}

#macro EyeBall(Side)
union {
	sphere { <0, 0, 0>, 1  scale <0.081, 0.063, 0.058>  texture { EyeWhite } }
	// scale は中心座標にも掛かる。先に潰してから位置を決める。
	sphere { <0, 0, 0>, 1  scale <0.040, 0.036, 0.030>
	         translate <0.015*Side, 0, -0.040>  texture { EyeBlack } }
}
#end

#declare FlipperUp = sphere_sweep {
	b_spline 6
	<-0.04, 1.02,  0.02>, 0.152
	<-0.30, 1.26,  0.00>, 0.148
	<-0.56, 1.66, -0.03>, 0.130
	<-0.63, 2.04, -0.05>, 0.100
	<-0.54, 2.26, -0.05>, 0.068
	<-0.43, 2.33, -0.04>, 0.028
	tolerance 0.00004
	scale <1, 1, 0.78>
	texture { Charcoal }
}

#declare FlipperDown = sphere_sweep {
	b_spline 6
	< 0.10, 1.16,  0.02>, 0.150
	< 0.38, 1.04,  0.00>, 0.148
	< 0.58, 0.78, -0.02>, 0.128
	< 0.63, 0.48, -0.04>, 0.102
	< 0.62, 0.27, -0.05>, 0.066
	< 0.58, 0.17, -0.06>, 0.028
	tolerance 0.00004
	scale <1, 1, 0.78>
	texture { Charcoal }
}

#declare Foot = sphere {
	<0, 0, 0>, 1
	scale <0.180, 0.056, 0.425>
	texture { Tangerine }
}

#declare Pingu = union {
	object { Torso }
	object { Head }
	object { BellyRim }
	object { Belly }
	object { Beak }
	object { EyeBall(-1)  rotate z* 21  rotate y*-13  translate <-0.131, 1.645, -0.246> }  // 左目は左下へ垂れる
	object { EyeBall( 1)  rotate z*-21  rotate y* 13  translate < 0.131, 1.645, -0.246> }  // 右目は右下へ垂れる
	object { FlipperUp }
	object { FlipperDown }
	object { Foot  rotate y* 17  translate <-0.215, 0.058, -0.190> }
	object { Foot  rotate y*-21  translate < 0.250, 0.058, -0.215> }
}

object { Pingu  rotate y*-9 }

