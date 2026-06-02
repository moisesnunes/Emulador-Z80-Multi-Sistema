; TEST.ASM — Simple CP/M program for XMODEM testing
; Assemble with: ASM TEST.ASM
; Link with: LOAD TEST.HEX
; Result: TEST.COM can be transferred via XMODEM

	ORG	0100H

; BDOS call = 0x0005 (JP to BDOS entry)
; Console Output (fn 2, C=2): prints char in E

	MVI	C,9		; fn 9 = print string
	LXI	D,MSG		; point to message
	CALL	0005		; BDOS call
	
	MVI	C,0		; fn 0 = system reset
	CALL	0005		; halt

MSG:	DB	'Hello from XMODEM transfer!$'
	
	END
