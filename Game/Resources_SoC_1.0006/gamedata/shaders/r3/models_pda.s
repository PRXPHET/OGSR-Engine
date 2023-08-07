function normal   (shader, t_base, t_second, t_detail)
	  shader:begin	("models_pda_screen","models_pda_screen")
		: fog			(true)
		: zb			(true,false)
		: blend			(true,blend.srcalpha,blend.invsrcalpha)
		: aref			(true,0)
		: sorting		(2,true)
		: distort		(true)
	shader:dx10texture	("s_base",	t_base)
	shader:dx10texture 	("s_vp2",	"$user$viewport2")	
	shader:dx10texture 	("s_load",	"ui\\ui_pda_loadscreen")	
	shader:dx10texture	("s_hud_rain",	"fx\\hud_rain")
	shader:dx10sampler	("smp_base")	
end
