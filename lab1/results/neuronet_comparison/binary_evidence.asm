; Selected read-only disassembly evidence for neuronet_3_SM.exe (see binary_evidence.json).
; Addresses are original virtual addresses. Disjoint excerpts have explicit section labels.
; llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel neuronet_3_SM.exe
; Data constants: 0x501df8 double 0.04; 0x501e00 float 5; 0x501e04 float 140;
; 0x501e08 float 30; 0x502974 float 10; 0x45d618 -> __moddi3 via IAT 0x51fc34.
; The executable itself is not included.

; Izhikevich calculation: inputs, Euler, threshold/reset and timer
  445d90:      	push	ebp
  445d91:      	mov	ebp, esp
  445d93:      	push	edi
  445d94:      	push	esi
  445d95:      	push	ebx
  445d96:      	sub	esp, 0x5c
  445d99:      	mov	ebx, dword ptr [ebp + 0x8]
  445d9c:      	mov	eax, dword ptr [ebx + 0x18]
  445d9f:      	mov	edx, dword ptr [ebx + 0x1c]
  445da2:      	mov	dword ptr [ebx + 0x20], eax
  445da5:      	mov	dword ptr [ebx + 0x24], edx
  445da8:      	mov	eax, dword ptr [ebx + 0x8]
  445dab:      	mov	edx, dword ptr [ebx + 0xc]
  445dae:      	mov	dword ptr [ebx + 0x10], eax
  445db1:      	mov	dword ptr [ebx + 0x14], edx
  445db4:      	mov	dword ptr [ebx + 0x1a0], 0x0
  445dbe:      	mov	dword ptr [ebx + 0x1a4], 0x0
  445dc8:      	cmp	byte ptr [ebx + 0x184], 0x0
  445dcf:      	je	0x445ed8 <.text+0x44ed8>
  445dd5:      	mov	eax, dword ptr [ebx + 0xb4]
  445ddb:      	mov	dword ptr [ebp - 0x20], eax
  445dde:      	mov	eax, dword ptr [ebx + 0xb8]
  445de4:      	sub	eax, dword ptr [ebp - 0x20]
  445de7:      	sar	eax, 0x2
  445dea:      	mov	dword ptr [ebp - 0x24], eax
  445ded:      	je	0x445ed8 <.text+0x44ed8>
  445df3:      	fldz
  445df5:      	xor	eax, eax
  445df7:      	mov	edx, dword ptr [ebx + 0xf0]
  445dfd:      	mov	dword ptr [ebp - 0x2c], edx
  445e00:      	mov	esi, dword ptr [ebx + 0xc0]
  445e06:      	mov	dword ptr [ebp - 0x28], esi
  445e09:      	jmp	0x445e64 <.text+0x44e64>
  445e0b:      	nop
  445e0c:      	mov	edx, dword ptr [ebp - 0x2c]
  445e0f:      	mov	ecx, dword ptr [edx + ecx]
  445e12:      	mov	esi, dword ptr [edi + 0x128]
  445e18:      	mov	edi, dword ptr [edi + 0x134]
  445e1e:      	mov	dword ptr [ebp - 0x4c], edi
  445e21:      	mov	edi, esi
  445e23:      	mov	edx, dword ptr [ebp - 0x3c]
  445e26:      	sub	edi, dword ptr [edx + 0x12c]
  445e2c:      	sar	edi, 0x3
  445e2f:      	mov	dword ptr [ebp - 0x3c], edi
  445e32:      	mov	edx, edi
  445e34:      	add	edx, ecx
  445e36:      	js	0x446064 <.text+0x45064>
  445e3c:      	cmp	edx, 0x3f
  445e3f:      	jg	0x446014 <.text+0x45014>
  445e45:      	lea	edx, [esi + 8*ecx]
  445e48:      	fld	qword ptr [edx]
  445e4a:      	mov	edi, dword ptr [ebp - 0x28]
  445e4d:      	fmul	qword ptr [edi + 8*eax]
  445e50:      	faddp	st(1), st
  445e52:      	fstp	qword ptr [ebx + 0x1a0]
  445e58:      	inc	eax
  445e59:      	cmp	eax, dword ptr [ebp - 0x24]
  445e5c:      	jae	0x445ecd <.text+0x44ecd>
  445e5e:      	fld	qword ptr [ebx + 0x1a0]
  445e64:      	lea	ecx, [4*eax]
  445e6b:      	mov	edi, dword ptr [ebp - 0x20]
  445e6e:      	mov	edi, dword ptr [edi + ecx]
  445e71:      	mov	dword ptr [ebp - 0x3c], edi
  445e74:      	cmp	byte ptr [edi + 0x178], 0x0
  445e7b:      	jne	0x445e0c <.text+0x44e0c>
  445e7d:      	mov	edx, dword ptr [ebp - 0x2c]
  445e80:      	mov	ecx, dword ptr [edx + ecx]
  445e83:      	mov	edi, dword ptr [ebp - 0x3c]
  445e86:      	mov	esi, dword ptr [edi + 0x128]
  445e8c:      	mov	edx, dword ptr [edi + 0x134]
  445e92:      	mov	dword ptr [ebp - 0x4c], edx
  445e95:      	mov	edx, esi
  445e97:      	sub	edx, dword ptr [edi + 0x12c]
  445e9d:      	sar	edx, 0x3
  445ea0:      	mov	dword ptr [ebp - 0x3c], edx
  445ea3:      	add	edx, ecx
  445ea5:      	js	0x446054 <.text+0x45054>
  445eab:      	cmp	edx, 0x3f
  445eae:      	jg	0x446034 <.text+0x45034>
  445eb4:      	lea	edx, [esi + 8*ecx]
  445eb7:      	fld	qword ptr [edx]
  445eb9:      	mov	edi, dword ptr [ebp - 0x28]
  445ebc:      	fmul	qword ptr [edi + 8*eax]
  445ebf:      	fsubp	st(1), st
  445ec1:      	fstp	qword ptr [ebx + 0x1a0]
  445ec7:      	inc	eax
  445ec8:      	cmp	eax, dword ptr [ebp - 0x24]
  445ecb:      	jb	0x445e5e <.text+0x44e5e>
  445ecd:      	fld	qword ptr [ebx + 0x1a0]
  445ed3:      	fstp	qword ptr [ebp - 0x20]
  445ed6:      	jmp	0x445edd <.text+0x44edd>
  445ed8:      	fldz
  445eda:      	fstp	qword ptr [ebp - 0x20]
  445edd:      	mov	eax, dword ptr [ebx + 0x194]
  445ee3:      	mov	dword ptr [esp], eax
  445ee6:      	call	0x431580 <.text+0x30580>
  445eeb:      	fadd	qword ptr [ebp - 0x20]
  445eee:      	fst	qword ptr [ebx + 0x1a0]
  445ef4:      	mov	ecx, dword ptr [ebx + 0x114]
  445efa:      	mov	esi, dword ptr [ebx + 0x118]
  445f00:      	sub	esi, ecx
  445f02:      	sar	esi, 0x2
  445f05:      	je	0x445f1d <.text+0x44f1d>
  445f07:      	xor	eax, eax
  445f09:      	lea	esi, [esi]
  445f0c:      	mov	edx, dword ptr [ecx + 4*eax]
  445f0f:      	fadd	qword ptr [edx + 0x38]
  445f12:      	inc	eax
  445f13:      	cmp	eax, esi
  445f15:      	jb	0x445f0c <.text+0x44f0c>
  445f17:      	fst	qword ptr [ebx + 0x1a0]
  445f1d:      	fld	qword ptr [ebx + 0x8]
  445f20:      	fld	qword ptr [ebx + 0x1c8]
  445f26:      	fld	qword ptr [ebx + 0x1b0]
  445f2c:      	fmul	st, st(2)
  445f2e:      	fsub	st, st(1)
  445f30:      	fmul	qword ptr [ebx + 0x1a8]
  445f36:      	mov	edx, dword ptr [ebx + 0xb0]
  445f3c:      	fld	qword ptr [0x501df8]
  445f42:      	fmul	st, st(3)
  445f44:      	fmul	st, st(3)
  445f46:      	fld	st(3)
  445f48:      	fmul	dword ptr [0x501e00]
  445f4e:      	faddp	st(1), st
  445f50:      	fadd	dword ptr [0x501e04]
  445f56:      	fsub	st, st(2)
  445f58:      	faddp	st(4), st
  445f5a:      	fxch	st(3)
  445f5c:      	fmul	qword ptr [edx + 0x990]
  445f62:      	faddp	st(2), st
  445f64:      	fxch	st(1)
  445f66:      	fst	qword ptr [ebx + 0x8]
  445f69:      	fxch	st(2)
  445f6b:      	fmul	qword ptr [edx + 0x990]
  445f71:      	faddp	st(1), st
  445f73:      	fst	qword ptr [ebx + 0x1c8]
  445f79:      	fld	dword ptr [0x501e08]
  445f7f:      	fld	st(0)
  445f81:      	fxch	st(3)
  445f83:      	fucomp	st(3)
  445f85:      	fnstsw	ax
  445f87:      	fstp	st(2)
  445f89:      	test	ah, 0x5
  445f8c:      	jne	0x445fbc <.text+0x44fbc>
  445f8e:      	fxch	st(1)
  445f90:      	mov	byte ptr [ebx + 0x180], 0x1
  445f97:      	mov	esi, dword ptr [ebx + 0x1b8]
  445f9d:      	mov	edi, dword ptr [ebx + 0x1bc]
  445fa3:      	mov	dword ptr [ebx + 0x8], esi
  445fa6:      	mov	dword ptr [ebx + 0xc], edi
  445fa9:      	fstp	qword ptr [ebx + 0x10]
  445fac:      	fadd	qword ptr [ebx + 0x1c0]
  445fb2:      	fstp	qword ptr [ebx + 0x1c8]
  445fb8:      	jmp	0x445fc0 <.text+0x44fc0>
  445fba:      	nop
  445fbc:      	fstp	st(0)
  445fbe:      	fstp	st(0)
  445fc0:      	fld	qword ptr [ebx + 0x40]
  445fc3:      	fldz
  445fc5:      	fld	st(0)
  445fc7:      	fxch	st(2)
  445fc9:      	fucom	st(2)
  445fcb:      	fnstsw	ax
  445fcd:      	and	ah, 0x45
  445fd0:      	cmp	ah, 0x40
  445fd3:      	je	0x445ff8 <.text+0x44ff8>
  445fd5:      	fsub	qword ptr [edx + 0x990]
  445fdb:      	fst	qword ptr [ebx + 0x40]
  445fde:      	fxch	st(2)
  445fe0:      	fucomp	st(2)
  445fe2:      	fnstsw	ax
  445fe4:      	fstp	st(1)
  445fe6:      	test	ah, 0x5
  445fe9:      	jne	0x446000 <.text+0x45000>
  445feb:      	mov	byte ptr [ebx + 0x180], 0x0
  445ff2:      	fstp	qword ptr [ebx + 0x40]
  445ff5:      	jmp	0x446002 <.text+0x45002>
  445ff7:      	nop
  445ff8:      	fstp	st(0)
  445ffa:      	fstp	st(0)
  445ffc:      	fstp	st(0)
  445ffe:      	jmp	0x446002 <.text+0x45002>
  446000:      	fstp	st(0)
  446002:      	mov	dword ptr [ebp + 0x8], ebx
  446005:      	add	esp, 0x5c
  446008:      	pop	ebx
  446009:      	pop	esi
  44600a:      	pop	edi
  44600b:      	leave
  44600c:      	jmp	0x417ec4 <.text+0x16ec4>

; UI Spike showing / serialized refractory_time use the same field
  4121d7:      	mov	edi, dword ptr [ebp - 0x28]
  4121da:      	mov	edx, dword ptr [esi + 0x38]
  4121dd:      	mov	ecx, dword ptr [esi + 0x3c]
  4121e0:      	mov	dword ptr [esp + 0x4], 0x4ff8ab
  4121e8:      	mov	dword ptr [esp], edi
  4121eb:      	mov	dword ptr [ebp - 0x30], edx
  4121ee:      	mov	dword ptr [ebp - 0x34], ecx
  4121f1:      	call	ebx
  4121f3:      	mov	edx, dword ptr [ebp - 0x30]
  4121f6:      	mov	ecx, dword ptr [ebp - 0x34]
  4121f9:      	mov	dword ptr [esp + 0x4], edx
  4121fd:      	mov	dword ptr [esp + 0x8], ecx
  412201:      	mov	dword ptr [esp], eax
  412204:      	call	dword ptr [ebp - 0x1c]
  412207:      	mov	dword ptr [esp + 0x4], 0x4ff81b
  41220f:      	mov	dword ptr [esp], eax
  412212:      	call	ebx
  447a32:      	mov	dword ptr [esp + 0x8], 0x0
  447a3a:      	mov	dword ptr [esp + 0x4], 0x4
  447a42:      	mov	eax, dword ptr [ebp + 0xc]
  447a45:      	mov	dword ptr [esp], eax
  447a48:      	lea	ebx, [ebp - 0x1c]
  447a4b:      	call	dword ptr [ebp - 0xdc]
  447a51:      	mov	esi, eax
  447a53:      	mov	dword ptr [esp + 0x4], 0xffffffff
  447a5b:      	mov	dword ptr [esp], 0x501ddf
  447a62:      	call	dword ptr [ebp - 0xe4]
  447a68:      	mov	dword ptr [ebp - 0x34], eax
  447a6b:      	mov	eax, dword ptr [esi]
  447a6d:      	mov	eax, dword ptr [eax + 0x10]
  447a70:      	mov	dword ptr [ebp - 0xec], eax
  447a76:      	lea	edi, [ebp - 0x34]
  447a79:      	mov	dword ptr [esp + 0x4], edi
  447a7d:      	lea	ebx, [ebp - 0x88]
  447a83:      	mov	dword ptr [esp], ebx
  447a86:      	call	dword ptr [ebp - 0xe0]
  447a8c:      	mov	dword ptr [esp + 0x8], ebx
  447a90:      	mov	dword ptr [esp + 0x4], 0x0
  447a98:      	mov	dword ptr [esp], esi
  447a9b:      	call	dword ptr [ebp - 0xec]
  447aa1:      	mov	dword ptr [esp], ebx
  447aa4:      	call	dword ptr [ebp - 0xe8]
  447aaa:      	mov	eax, dword ptr [ebp - 0x34]
  447aad:      	lock
  447aae:      	dec	dword ptr [eax]
  447ab0:      	setne	dl
  447ab3:      	test	dl, dl
  447ab5:      	je	0x447d98 <.text+0x46d98>
  447ced:      	mov	dword ptr [esp + 0x10], 0x6
  447cf5:      	mov	dword ptr [esp + 0xc], 0x67
  447cfd:      	mov	ecx, dword ptr [ebp + 0x8]
  447d00:      	mov	eax, dword ptr [ecx + 0x38]
  447d03:      	mov	edx, dword ptr [ecx + 0x3c]
  447d06:      	mov	dword ptr [esp + 0x4], eax
  447d0a:      	mov	dword ptr [esp + 0x8], edx
  447d0e:      	mov	dword ptr [esp], ebx
  447d11:      	call	dword ptr [ebp - 0xe4]
  447d17:      	mov	dword ptr [esp + 0x8], 0x1
  447d1f:      	mov	dword ptr [esp + 0x4], 0x4
  447d27:      	mov	eax, dword ptr [ebp + 0xc]
  447d2a:      	mov	dword ptr [esp], eax
  447d2d:      	call	dword ptr [ebp - 0xdc]
  447d33:      	mov	edi, eax
  447d35:      	mov	eax, dword ptr [eax]
  447d37:      	mov	eax, dword ptr [eax + 0x10]
  447d3a:      	mov	dword ptr [ebp - 0xf0], eax
  447d40:      	mov	dword ptr [esp + 0x4], ebx
  447d44:      	lea	esi, [ebp - 0xd8]
  447d4a:      	mov	dword ptr [esp], esi
  447d4d:      	call	dword ptr [ebp - 0xe0]
  447d53:      	mov	dword ptr [esp + 0x8], esi
  447d57:      	mov	dword ptr [esp + 0x4], 0x0
  447d5f:      	mov	dword ptr [esp], edi
  447d62:      	call	dword ptr [ebp - 0xf0]

; Event gate and Frequency counter including its default 50 ms window
  417fb3:      	fld	qword ptr [ebx + 0x40]
  417fb6:      	fldz
  417fb8:      	fxch	st(1)
  417fba:      	fucompp
  417fbc:      	fnstsw	ax
  417fbe:      	and	ah, 0x45
  417fc1:      	xor	ah, 0x40
  417fc4:      	jne	0x417fd3 <.text+0x16fd3>
  417fc6:      	cmp	byte ptr [ebx + 0x180], 0x0
  417fcd:      	jne	0x4185fa <.text+0x175fa>
  417fd3:      	mov	eax, dword ptr [ebx + 0x128]
  417fd9:      	cmp	eax, dword ptr [ebx + 0x12c]
  417fdf:      	je	0x418992 <.text+0x17992>
  417fe5:      	mov	edx, eax
  417fe7:      	sub	edx, 0x8
  417fea:      	je	0x418001 <.text+0x17001>
  417fec:      	mov	edx, dword ptr [ebx + 0x18]
  417fef:      	mov	ecx, dword ptr [ebx + 0x1c]
  417ff2:      	mov	dword ptr [eax - 0x8], edx
  417ff5:      	mov	dword ptr [eax - 0x4], ecx
  417ff8:      	mov	edx, dword ptr [ebx + 0x128]
  417ffe:      	sub	edx, 0x8
  418001:      	mov	dword ptr [ebx + 0x128], edx
  418007:      	mov	eax, dword ptr [ebx + 0x138]
  41800d:      	cmp	eax, dword ptr [ebx + 0x13c]
  418013:      	je	0x418a17 <.text+0x17a17>
  418019:      	sub	eax, 0x8
  41801c:      	mov	dword ptr [ebx + 0x138], eax
  418022:      	mov	esi, dword ptr [ebx + 0x50]
  418025:      	mov	eax, dword ptr [ebx + 0x58]
  418028:      	mov	dword ptr [ebp - 0x70], eax
  41802b:      	mov	dword ptr [ebp - 0x80], eax
  41802e:      	mov	edx, dword ptr [ebx + 0x5c]
  418031:      	mov	dword ptr [ebp - 0x6c], edx
  418034:      	mov	dword ptr [ebp - 0x84], edx
  41803a:      	mov	ecx, esi
  41803c:      	mov	edi, dword ptr [ebx + 0xb0]
  418042:      	mov	eax, dword ptr [ebx + 0x60]
  418045:      	mov	dword ptr [ebp - 0x78], eax
  418048:      	mov	edx, dword ptr [ebx + 0x6c]
  41804b:      	mov	dword ptr [ebp - 0x74], edx
  41804e:      	mov	eax, dword ptr [ebx + 0x64]
  418051:      	mov	dword ptr [ebp - 0x7c], eax
  418054:      	mov	edx, dword ptr [ebp - 0x70]
  418057:      	sub	edx, ecx
  418059:      	sar	edx, 0x3
  41805c:      	mov	eax, dword ptr [ebp - 0x78]
  41805f:      	sub	eax, dword ptr [ebp - 0x7c]
  418062:      	sar	eax, 0x3
  418065:      	add	edx, eax
  418067:      	mov	eax, dword ptr [ebp - 0x74]
  41806a:      	sub	eax, dword ptr [ebp - 0x6c]
  41806d:      	sar	eax, 0x2
  418070:      	shl	eax, 0x6
  418073:      	lea	edx, [eax + edx - 0x40]
  418077:      	test	edx, edx
  418079:      	je	0x418380 <.text+0x17380>
  41807f:      	fld	qword ptr [edi + 0x9e8]
  418085:      	fld	qword ptr [edi + 0x998]
  41808b:      	fsub	st, st(1)
  41808d:      	fld	qword ptr [esi]
  41808f:      	fxch	st(1)
  418091:      	fucompp
  418093:      	fnstsw	ax
  418095:      	test	ah, 0x45
  418098:      	je	0x41838c <.text+0x1738c>
  41809e:      	lea	eax, [edx + 4*edx]
  4180a1:      	lea	eax, [eax + 4*eax]
  4180a4:      	lea	eax, [eax + 4*eax]
  4180a7:      	shl	eax, 0x3
  4180aa:      	xor	edx, edx
  4180ac:      	mov	dword ptr [ebp - 0x60], eax
  4180af:      	mov	dword ptr [ebp - 0x5c], edx
  4180b2:      	fild	qword ptr [ebp - 0x60]
  4180b5:      	fdivrp	st(1), st
  4180b7:      	fstp	qword ptr [ebx + 0x188]
  41838c:      	fstp	st(0)
  41838e:      	mov	eax, dword ptr [ebp - 0x70]
  418391:      	sub	eax, 0x8
  418394:      	cmp	ecx, eax
  418396:      	je	0x418414 <.text+0x17414>
  418398:      	add	ecx, 0x8
  41839b:      	mov	dword ptr [ebx + 0x50], ecx
  41839e:      	add	esi, 0x8
  4183a1:      	cmp	dword ptr [ebp - 0x80], esi
  4183a4:      	je	0x4183f8 <.text+0x173f8>
  4183a6:      	mov	ecx, dword ptr [ebx + 0x50]
  4183a9:      	jmp	0x418054 <.text+0x17054>
  4183ae:      	nop
  41865c:      	mov	eax, dword ptr [ebx + 0x60]
  41865f:      	mov	edx, dword ptr [ebx + 0x68]
  418662:      	sub	edx, 0x8
  418665:      	cmp	eax, edx
  418667:      	je	0x418dac <.text+0x17dac>
  41866d:      	test	eax, eax
  41866f:      	je	0x41868b <.text+0x1768b>
  418671:      	mov	edx, dword ptr [edi + 0x998]
  418677:      	mov	ecx, dword ptr [edi + 0x99c]
  41867d:      	mov	dword ptr [eax], edx
  41867f:      	mov	dword ptr [eax + 0x4], ecx
  418682:      	mov	eax, dword ptr [ebx + 0x60]
  418685:      	mov	edi, dword ptr [ebx + 0xb0]
  41868b:      	add	eax, 0x8
  41868e:      	mov	dword ptr [ebx + 0x60], eax
  418691:      	mov	esi, dword ptr [ebx + 0x190]
  418697:      	test	esi, esi
  418699:      	je	0x4186d0 <.text+0x176d0>
  41869b:      	mov	eax, dword ptr [esi + 0x38]
  41869e:      	mov	edx, dword ptr [esi + 0x40]
  4186a1:      	sub	edx, 0x8
  4186a4:      	cmp	eax, edx
  4186a6:      	je	0x418ea4 <.text+0x17ea4>
  4186ac:      	test	eax, eax
  4186ae:      	je	0x4186ca <.text+0x176ca>
  4186b0:      	mov	edx, dword ptr [edi + 0x998]
  4186b6:      	mov	ecx, dword ptr [edi + 0x99c]
  4186bc:      	mov	dword ptr [eax], edx
  4186be:      	mov	dword ptr [eax + 0x4], ecx
  4186c1:      	mov	eax, dword ptr [esi + 0x38]
  4186c4:      	mov	edi, dword ptr [ebx + 0xb0]
  4186ca:      	add	eax, 0x8
  4186cd:      	mov	dword ptr [esi + 0x38], eax
  4186d0:      	cmp	byte ptr [edi + 0x97e], 0x0
  4186d7:      	jne	0x418a7c <.text+0x17a7c>
  4186dd:      	mov	eax, dword ptr [ebx + 0x38]
  4186e0:      	mov	edx, dword ptr [ebx + 0x3c]
  4186e3:      	mov	dword ptr [ebx + 0x40], eax
  4186e6:      	mov	dword ptr [ebx + 0x44], edx
  41eab8:      	mov	eax, dword ptr [ebx + 0x18]
  41eabb:      	mov	edx, dword ptr [eax + 0x18c]
  41eac1:      	mov	eax, dword ptr [eax + 0x188]
  41eac7:      	mov	dword ptr [esp + 0x4], eax
  41eacb:      	mov	dword ptr [esp + 0x8], edx
  41eacf:      	mov	eax, dword ptr [ebx + 0x14]
  41ead2:      	mov	eax, dword ptr [eax + 0x94]
  41ead8:      	mov	dword ptr [esp], eax
  41eadb:      	call	dword ptr [0x51fdc0]
  4253ab:      	mov	dword ptr [ebx + 0x990], 0x0
  4253b5:      	mov	dword ptr [ebx + 0x994], 0x3fe00000
  4253bf:      	mov	dword ptr [ebx + 0x9a0], 0x0
  4253c9:      	mov	dword ptr [ebx + 0x9a4], 0x0
  4253d3:      	mov	dword ptr [ebx + 0x9a8], 0x0
  4253dd:      	mov	dword ptr [ebx + 0x9ac], 0x40240000
  4253e7:      	mov	dword ptr [ebx + 0x9b0], 0x9999999a
  4253f1:      	mov	dword ptr [ebx + 0x9b4], 0x3fa99999
  4253fb:      	mov	dword ptr [ebx + 0x9e8], 0x0
  425405:      	mov	dword ptr [ebx + 0x9ec], 0x40490000

; Normal rhythmic stimulation: phase and inclusive interval end
  44b880:      	push	ebp
  44b881:      	mov	ebp, esp
  44b883:      	push	edi
  44b884:      	push	esi
  44b885:      	push	ebx
  44b886:      	sub	esp, 0x5c
  44b889:      	mov	ebx, dword ptr [ebp + 0x8]
  44b88c:      	mov	eax, dword ptr [ebx + 0x38]
  44b88f:      	mov	edx, dword ptr [ebx + 0x3c]
  44b892:      	mov	dword ptr [ebx + 0x40], eax
  44b895:      	mov	dword ptr [ebx + 0x44], edx
  44b898:      	mov	dword ptr [ebx + 0x38], 0x0
  44b89f:      	mov	dword ptr [ebx + 0x3c], 0x0
  44b8a6:      	cmp	byte ptr [ebx + 0x14], 0x0
  44b8aa:      	je	0x44bb0a <.text+0x4ab0a>
  44b8b0:      	mov	edx, dword ptr [ebx + 0x10]
  44b8b3:      	cmp	byte ptr [edx + 0xaac], 0x0
  44b8ba:      	je	0x44ba28 <.text+0x4aa28>
  44ba28:      	cmp	byte ptr [ebx + 0x71], 0x0
  44ba2c:      	jne	0x44ba38 <.text+0x4aa38>
  44ba2e:      	cmp	byte ptr [ebx + 0x72], 0x0
  44ba32:      	jne	0x44bb14 <.text+0x4ab14>
  44ba38:      	mov	eax, dword ptr [ebx + 0x10]
  44ba3b:      	fld	dword ptr [0x502974]
  44ba41:      	fld	qword ptr [eax + 0x998]
  44ba47:      	fmul	st, st(1)
  44ba49:      	fnstcw	word ptr [ebp - 0x2a]
  44ba4c:      	mov	ax, word ptr [ebp - 0x2a]
  44ba50:      	mov	ah, 0xc
  44ba52:      	mov	word ptr [ebp - 0x2c], ax
  44ba56:      	fldcw	word ptr [ebp - 0x2c]
  44ba59:      	fistp	qword ptr [esp]
  44ba5c:      	fldcw	word ptr [ebp - 0x2a]
  44ba5f:      	mov	eax, dword ptr [esp]
  44ba62:      	mov	edx, dword ptr [esp + 0x4]
  44ba66:      	mov	dword ptr [ebx], eax
  44ba68:      	mov	dword ptr [ebx + 0x4], edx
  44ba6b:      	fld	qword ptr [ebx + 0x30]
  44ba6e:      	fldcw	word ptr [ebp - 0x2c]
  44ba71:      	fistp	qword ptr [ebp - 0x38]
  44ba74:      	fldcw	word ptr [ebp - 0x2a]
  44ba77:      	mov	esi, dword ptr [ebp - 0x38]
  44ba7a:      	mov	edi, dword ptr [ebp - 0x34]
  44ba7d:      	mov	eax, esi
  44ba7f:      	mov	edx, edi
  44ba81:      	add	eax, esi
  44ba83:      	adc	edx, edi
  44ba85:      	shld	edi, esi, 0x3
  44ba89:      	shl	esi, 0x3
  44ba8c:      	add	eax, esi
  44ba8e:      	adc	edx, edi
  44ba90:      	mov	dword ptr [esp + 0x8], eax
  44ba94:      	mov	dword ptr [esp + 0xc], edx
  44ba98:      	fstp	qword ptr [ebp - 0x58]
  44ba9b:      	call	0x45d618 <.text+0x5c618>
  44baa0:      	mov	dword ptr [ebp - 0x38], eax
  44baa3:      	mov	dword ptr [ebp - 0x34], edx
  44baa6:      	fild	qword ptr [ebp - 0x38]
  44baa9:      	fld	qword ptr [ebp - 0x58]
  44baac:      	fdivp	st(1), st
  44baae:      	fst	qword ptr [ebx + 0x8]
  44bab1:      	mov	esi, dword ptr [ebx + 0x48]
  44bab4:      	mov	edi, dword ptr [ebx + 0x4c]
  44bab7:      	sub	edi, esi
  44bab9:      	sar	edi, 0x3
  44babc:      	je	0x44bc41 <.text+0x4ac41>
  44bac2:      	fld	qword ptr [esi]
  44bac4:      	fucomp	st(1)
  44bac6:      	fnstsw	ax
  44bac8:      	test	ah, 0x5
  44bacb:      	je	0x44bc45 <.text+0x4ac45>
  44bad1:      	xor	edx, edx
  44bad3:      	jmp	0x44baeb <.text+0x4aaeb>
  44bad5:      	lea	esi, [esi]
  44bad8:      	lea	ecx, [8*edx]
  44badf:      	fld	qword ptr [esi + ecx]
  44bae2:      	fucomp	st(1)
  44bae4:      	fnstsw	ax
  44bae6:      	test	ah, 0x5
  44bae9:      	je	0x44bafc <.text+0x4aafc>
  44baeb:      	inc	edx
  44baec:      	cmp	edx, edi
  44baee:      	jb	0x44bad8 <.text+0x4aad8>
  44baf0:      	fstp	st(0)
  44baf2:      	lea	ecx, [8*edx]
  44baf9:      	jmp	0x44bafe <.text+0x4aafe>
  44bafb:      	nop
  44bafc:      	fstp	st(0)
  44bafe:      	mov	eax, dword ptr [ebx + 0x54]
  44bb01:      	fld	qword ptr [ebx + 0x38]
  44bb04:      	fadd	qword ptr [eax + ecx]
  44bb07:      	fstp	qword ptr [ebx + 0x38]
  44bb0a:      	lea	esp, [ebp - 0xc]
  44bb0d:      	pop	ebx
  44bb0e:      	pop	esi
  44bb0f:      	pop	edi
  44bb10:      	leave
  44bb11:      	ret
  44bc41:      	fstp	st(0)
  44bc43:      	jmp	0x44bc47 <.text+0x4ac47>
  44bc45:      	fstp	st(0)
  44bc47:      	xor	ecx, ecx
  44bc49:      	jmp	0x44bafe <.text+0x4aafe>

; Main iteration: stimulus, neuron, clock increment
  4277f7:      	mov	eax, dword ptr [ebp - 0x94]
  4277fd:      	mov	dword ptr [esp], eax
  427800:      	call	0x44b144 <.text+0x4a144>
  427805:      	mov	edx, dword ptr [ebx + 0x828]
  42780b:      	mov	eax, dword ptr [ebx + 0x82c]
  427811:      	sub	eax, edx
  427813:      	shr	eax, 0x2
  427816:      	je	0x42783d <.text+0x2683d>
  427818:      	xor	esi, esi
  42781a:      	nop
  42781c:      	mov	eax, dword ptr [edx + 4*esi]
  42781f:      	mov	dword ptr [esp], eax
  427822:      	call	0x44b880 <.text+0x4a880>
  427827:      	inc	esi
  427828:      	mov	edx, dword ptr [ebx + 0x828]
  42782e:      	mov	eax, dword ptr [ebx + 0x82c]
  427834:      	sub	eax, edx
  427836:      	sar	eax, 0x2
  427839:      	cmp	esi, eax
  42783b:      	jb	0x42781c <.text+0x2681c>
  42783d:      	cmp	byte ptr [ebx + 0x97a], 0x0
  427844:      	je	0x427862 <.text+0x26862>
  427846:      	fld	qword ptr [ebx + 0x9c0]
  42784c:      	fld	qword ptr [ebx + 0x990]
  427852:      	fmul	st, st(1)
  427854:      	fdiv	qword ptr [ebx + 0x9c8]
  42785a:      	fsubp	st(1), st
  42785c:      	fstp	qword ptr [ebx + 0x9c0]
  427862:      	mov	eax, dword ptr [ebx + 0x810]
  427868:      	mov	edx, dword ptr [ebx + 0x814]
  42786e:      	sub	edx, eax
  427870:      	shr	edx, 0x2
  427873:      	je	0x428099 <.text+0x27099>
  427879:      	xor	esi, esi
  42787b:      	jmp	0x427882 <.text+0x26882>
  42787d:      	lea	esi, [esi]
  427880:      	fstp	st(0)
  427882:      	lea	edi, [4*esi]
  427889:      	mov	eax, dword ptr [eax + edi]
  42788c:      	mov	edx, dword ptr [eax]
  42788e:      	mov	dword ptr [esp], eax
  427891:      	call	dword ptr [edx + 0x10]
  427894:      	mov	eax, dword ptr [ebx + 0x810]
  42789a:      	mov	edx, dword ptr [eax + edi]
  42789d:      	fld	qword ptr [ebx + 0x9e0]
  4278a3:      	fadd	qword ptr [edx + 0x188]
  4278a9:      	fst	qword ptr [ebx + 0x9e0]
  4278af:      	inc	esi
  4278b0:      	mov	edx, dword ptr [ebx + 0x814]
  4278b6:      	sub	edx, eax
  4278b8:      	sar	edx, 0x2
  4278bb:      	cmp	esi, edx
  4278bd:      	jb	0x427880 <.text+0x26880>
  427c26:      	mov	eax, dword ptr [ebx + 0xaa8]
  427c2c:      	mov	dword ptr [esp], eax
  427c2f:      	call	dword ptr [0x51fc8c]
  427c35:      	fld	qword ptr [ebx + 0x998]
  427c3b:      	fadd	qword ptr [ebx + 0x990]
  427c41:      	fst	qword ptr [ebx + 0x998]
  427c47:      	fldz
  427c49:      	fld	qword ptr [ebp - 0x58]
  427c4c:      	fucom	st(1)
  427c4e:      	fnstsw	ax
  427c50:      	fstp	st(1)
  427c52:      	and	ah, 0x45
  427c55:      	cmp	ah, 0x40
  427c58:      	je	0x427c67 <.text+0x26c67>
  427c5a:      	fld	qword ptr [ebx + 0x990]
