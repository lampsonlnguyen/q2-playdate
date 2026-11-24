.syntax unified
.cpu cortex-m7
.thumb

.global eventHandlerShim
.type eventHandlerShim, %function
.thumb_func
eventHandlerShim:
    cmp r1, #0
    bne rejected
    /* Make a representative amount of simple ALU/branch work per tick. */
    movw r1, #10000
    movs r0, #0
loop:
    adds r0, r0, r1
    subs r1, r1, #1
    bne loop
    bx lr
rejected:
    movs r0, #0
    bx lr
.size eventHandlerShim, .-eventHandlerShim
