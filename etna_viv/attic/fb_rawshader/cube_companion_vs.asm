MOV t3.xy__, void, void, u4.wwww
MOV t3.__z_, void, void, u5.wwww
MUL t4, u0, t0.xxxx, void
MAD t4, u1, t0.yyyy, t4
MAD t4, u2, t0.zzzz, t4
MAD t4, u3, t0.wwww, t4
MUL t5.xyz_, u4.xyzz, t1.xxxx, void
MAD t5.xyz_, u5.xyzz, t1.yyyy, t5.xyzz
MAD t1.xyz_, u6.xyzz, t1.zzzz, t5.xyzz
MUL t5, u7, t0.xxxx, void
MAD t5, u8, t0.yyyy, t5
MAD t5, u9, t0.zzzz, t5
MAD t0, u10, t0.wwww, t5
RCP t1.___w, void, void, t0.wwww
MAD t0.xyz_, -t0.xyzz, t1.wwww, t3.xyzz
DP3 t3.xyz_, t0.xyzz, t0.xyzz, void
RSQ t3.xyz_, void, void, t3.xxxx
MUL t0.xyz_, t0.xyzz, t3.xyzz, void
DP3 t0.x___, t1.xyzz, t0.xyzz, void
SELECT.LT t0.xyz_, u6.wwww, t0.xxxx, u6.wwww
MOV t0.___w, void, void, u11.xxxx
MOV t1.xy__, void, void, t2.xyyy
ADD t4.__z_, t4.zzzz, void, t4.wwww
MUL t4.__z_, t4.zzzz, u11.yyyy, void
