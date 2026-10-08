; IMA ADPCM decoder for the Falcon DSP.
;
; The DMA plays an APM file (see wav2apm.py) into the SSI receiver in
; handshake mode; the decoded samples are sent from a ring buffer to the
; DAC by the SSI transmit interrupt.
;
; Each received word is skewed by two bits, i.e. the top two bits of a word
; arrive at the end of the previous one; read_word undoes it (the same way
; as NoCrew's MP2 player does).
;
; Registers reserved for the transmit interrupt: r7, m7.

; --------------------------------------------------------------
;		Equates
; --------------------------------------------------------------

M_PCC		equ	$ffe1				; Port C Control Register
M_PCDDR		equ	$ffe3				; Port C Data Direction Register
M_PCD		equ	$ffe5				; Port C Data Register
M_CRA		equ	$ffec				; SSI Control Register A
M_CRB		equ	$ffed				; SSI Control Register B
M_SR		equ	$ffee				; SSI Status Register
M_RX		equ	$ffef				; SSI Receive Data Register
M_TX		equ	$ffef				; SSI Transmit Data Register
M_IPR		equ	$ffff				; Interrupt Priority Register

RING_SIZE	equ	1024				; output ring (power of two)
RING_LOW	equ	RING_SIZE/2			; fill level before playback starts
RING_HIGH	equ	RING_SIZE-8			; max fill level (room for one word)

APM_MAGIC_HI	equ	$4150				; "AP"
APM_MAGIC_LO	equ	$434d				; "CM"
APM_HEADER_WORDS equ	16

MAX_STEP_INDEX	equ	88

; --------------------------------------------------------------
;		Vectors
; --------------------------------------------------------------

	org	p:$0000
	jmp	>start

	org	p:$0010					; SSI transmit data
	movep	y:(r7)+,x:<<M_TX

	org	p:$0012					; SSI transmit data with exception
	movep	y:(r7)+,x:<<M_TX

; --------------------------------------------------------------
;		Main
; --------------------------------------------------------------

	org	p:$0040

start:
	ori	#$03,mr					; no interrupts until the ring is filled

	clr	a	#ring,r0
	do	#RING_SIZE,_clear
	move	a,y:(r0)+
_clear:
	move	#ring,r6
	move	#ring,r7
	movec	#RING_SIZE-1,m6
	movec	#RING_SIZE-1,m7

	jsr	setup_ssi
	jsr	read_header

	jclr	#1,x:<channels,mono

; Stereo block: each data word holds L1 L2 R1 R2 (4 bits each, MSB first),
; i.e. frames (L1, R1) and (L2, R2).
stereo:
	move	#<left_state,r4
	jsr	read_channel_header
	move	#<right_state,r4
	jsr	read_channel_header

	jsr	wait_space				; the header samples
	move	x:<left_state+1,b
	rep	#8
	lsl	b
	move	b1,y:(r6)+
	move	x:<right_state+1,b
	rep	#8
	lsl	b
	move	b1,y:(r6)+

	do	x:<data_words,_end
	jsr	wait_space
	jsr	read_word				; y0: word
	move	#>$000800,x0				; L1: >> 12
	mpy	y0,x0,a	#<left_state,r4
	jsr	decode_nibble
	move	#>$080000,x0				; R1: >> 4
	mpy	y0,x0,a	#<right_state,r4
	jsr	decode_nibble
	move	#>$008000,x0				; L2: >> 8
	mpy	y0,x0,a	#<left_state,r4
	jsr	decode_nibble
	tfr	y0,a	#<right_state,r4		; R2
	jsr	decode_nibble
	nop
_end:
	jmp	stereo

; Mono block: each data word holds four consecutive samples (4 bits each,
; MSB first), played on both channels.
mono:
	move	#<left_state,r4
	jsr	read_channel_header

	jsr	wait_space				; the header sample
	move	x:<left_state+1,b
	rep	#8
	lsl	b
	move	b1,y:(r6)+
	move	b1,y:(r6)+

	do	x:<data_words,_end
	jsr	wait_space
	jsr	read_word				; y0: word
	move	#>$000800,x0				; >> 12
	mpy	y0,x0,a	#<left_state,r4
	jsr	decode_nibble
	move	b1,y:(r6)+
	move	#>$008000,x0				; >> 8
	mpy	y0,x0,a	#<left_state,r4
	jsr	decode_nibble
	move	b1,y:(r6)+
	move	#>$080000,x0				; >> 4
	mpy	y0,x0,a	#<left_state,r4
	jsr	decode_nibble
	move	b1,y:(r6)+
	tfr	y0,a	#<left_state,r4
	jsr	decode_nibble
	move	b1,y:(r6)+
_end:
	jmp	mono

; --------------------------------------------------------------
;		Input
; --------------------------------------------------------------

; Read one word from the DMA (handshake) and undo the two-bit skew.
; out:  y0 = a1 = word (16 bits)
; uses: a, x0, x1
read_word:
	bset	#4,x:<<M_PCD				; frame sync for the handshake
	jclr	#7,x:<<M_SR,*				; wait for receive data full
	bclr	#4,x:<<M_PCD
	movep	x:<<M_RX,a				; a1: word in bits 23..8

	lsr	a					; insert the two bits saved
	lsr	a	x:<rx_bits,x0			; from the previous word
	or	x0,a	#>$010000,x1
	lsr	a
	move	a1,x0
	mpy	x0,x1,a	#>$ffff,x0			; a1 = x0 >> 7, a0[23:22]: bits for the next word
	and	x0,a	a0,x:<rx_bits
	move	a1,y0
	rts

; Find the APM header in the stream and read the stream parameters.
read_header:
	move	#0,y1					; previous word
_search:
	move	y1,b
	jsr	read_word
	move	y0,y1
	move	#>APM_MAGIC_LO,a
	cmp	y0,a
	jne	_search
	move	#>$3fff,x0				; the top two bits of the first
	and	x0,b					; received word may be lost
	move	#>APM_MAGIC_HI&$3fff,x0
	cmp	x0,b
	jne	_search

	jsr	read_word				; version
	jsr	read_word				; channels
	move	y0,x:<channels
	jsr	read_word				; rate (high)
	jsr	read_word				; rate (low)
	jsr	read_word				; block size (bytes)

	move	y0,a					; data words per block =
	lsr	a	x:<channels,x0			; block size / 2 - 2 * channels
	sub	x0,a
	sub	x0,a
	move	a1,x:<data_words

	do	#APM_HEADER_WORDS-7,_skip		; the rest of the header
	jsr	read_word
	nop
_skip:
	rts

; Read the state of one channel from a block header.
; in: r4 = channel state (step index, previous value)
read_channel_header:
	jsr	read_word				; previous value (signed 16 bits)
	move	#>$008000,x0
	eor	x0,a
	sub	x0,a	(r4)+				; sign extended
	move	a1,x:(r4)-
	jsr	read_word				; step index << 8
	move	#>$008000,x0				; >> 8
	mpy	y0,x0,a
	move	a1,x:(r4)
	rts

; --------------------------------------------------------------
;		Output
; --------------------------------------------------------------

; Wait until there is room for one data word in the output ring;
; start the playback once the ring has been filled for the first time.
; uses: a, x0
wait_space:
	move	r6,a
	move	r7,x0
	sub	x0,a	#>RING_SIZE-1,x0
	and	x0,a
	move	a1,a					; a: fill level
	jset	#0,x:<playing,_playing
	move	#>RING_LOW,x0
	cmp	x0,a
	jlt	_done
	bset	#0,x:<playing
	andi	#$fc,mr					; start the transmit interrupt
_playing:
	move	#>RING_HIGH,x0
	cmp	x0,a
	jge	wait_space
_done:
	rts

; Decode one 4-bit code (Jack Jansen's IMA ADPCM arithmetic) and append
; the sample to the output ring.
; in:   a1 = code in bits 3..0, r4 = channel state (step index, previous value)
; out:  b1 = sample << 8
; uses: a, b, x0, x1, y1, r0, r1, n0, n1
decode_nibble:
	move	#>$f,x1
	and	x1,a	#step_table,r1
	move	a1,y1					; y1: code
	move	x:(r4)+,n1				; n1: step index
	move	y1,n0					; n0: code
	move	x:(r1+n1),b				; b: step

	tfr	b,a
	rep	#3
	lsr	a					; a: step >> 3
	jclr	#2,y1,_no4
	add	b,a					; + step
_no4:
	lsr	b
	jclr	#1,y1,_no2
	add	b,a					; + step >> 1
_no2:
	lsr	b
	jclr	#0,y1,_no1
	add	b,a					; + step >> 2
_no1:
	move	x:(r4),b				; b: previous value
	jclr	#3,y1,_add
	neg	a
_add:
	add	a,b	#>32767,x0			; b: new value, clamped to 16 bits
	cmp	x0,b
	tgt	x0,b
	move	#>-32768,x0
	cmp	x0,b
	tlt	x0,b
	move	b1,x:(r4)-

	move	#index_table,r0
	move	x:(r4),a				; a: new step index, clamped
	move	x:(r0+n0),x0
	add	x0,a	#0,x0
	cmp	x0,a
	tlt	x0,a
	move	#>MAX_STEP_INDEX,x0
	cmp	x0,a
	tgt	x0,a
	move	a1,x:(r4)

	rep	#8
	lsl	b
	move	b1,y:(r6)+
	rts

; --------------------------------------------------------------
;		SSI setup
; --------------------------------------------------------------

setup_ssi:
	movep	#0,x:<<M_PCC				; reset the SSI (port C as GPIO)

	; 16-bit words, 2 words per frame (network mode)
	movep	#$004100,x:<<M_CRA

	; TIE, RE, TE, network mode, asynchronous, continuous clock,
	; word length frame sync, MSB first, external clocks, SC1 as output
	movep	#$007808,x:<<M_CRB

	; SSI function for STD, SRD, SCK and SC2; PC3 (SC0) and PC4 (SC1)
	; stay GPIO, PC4 is the receive frame sync driven by read_word
	movep	#$0001e0,x:<<M_PCC
	movep	#$000010,x:<<M_PCDDR
	movep	#0,x:<<M_PCD

	movep	#$003000,x:<<M_IPR			; SSI interrupts at IPL 2
	rts

	if	@cvi(*)>$0200
	fail	'Program does not fit into the internal P memory'
	endif

; --------------------------------------------------------------
;		Data
; --------------------------------------------------------------

	org	x:$0000

channels	dc	0
data_words	dc	0				; per block
rx_bits		dc	0				; skewed bits for the next word
playing		dc	0

left_state	dc	0,0				; step index, previous value
right_state	dc	0,0

index_table:
	dc	-1,-1,-1,-1,2,4,6,8
	dc	-1,-1,-1,-1,2,4,6,8

step_table:
	dc	7,8,9,10,11,12,13,14,16,17
	dc	19,21,23,25,28,31,34,37,41,45
	dc	50,55,60,66,73,80,88,97,107,118
	dc	130,143,157,173,190,209,230,253,279,307
	dc	337,371,408,449,494,544,598,658,724,796
	dc	876,963,1060,1166,1282,1411,1552,1707,1878,2066
	dc	2272,2499,2749,3024,3327,3660,4026,4428,4871,5358
	dc	5894,6484,7132,7845,8630,9493,10442,11487,12635,13899
	dc	15289,16818,18500,20350,22385,24623,27086,29794,32767

	if	@cvi(*)>$0100
	fail	'Data does not fit into the internal X memory'
	endif

	org	y:$0400					; external, aligned to RING_SIZE
ring	ds	RING_SIZE
