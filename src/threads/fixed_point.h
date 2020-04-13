#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

typedef int64_t fixed_t;

#define FP_SHIFT_AMOUNT 16

#define CONVERT_TO_FP(n) ((fixed_t)(n << FP_SHIFT_AMOUNT))

#define CONVERT_TO_INT(n) (n >> FP_SHIFT_AMOUNT)

#define CONVERT_TO_INT_ROUND(n) (n >= 0 ? ((n + (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT) \
        : ((n - (1 << (FP_SHIFT_AMOUNT - 1))) >> FP_SHIFT_AMOUNT))

#define ADD(a, b) (a + b)

#define ADD_INT(a, b) (a + (b << FP_SHIFT_AMOUNT))

#define SUB(a, b) (a - b)

#define SUB_INT(a, b) (a - (b << FP_SHIFT_AMOUNT))

#define MULT_INT(a, b) (a * b)

#define DIV_INT(a, b) (a / b)

#define MULT(a, b) ((fixed_t)(((int64_t) a) * b >> FP_SHIFT_AMOUNT))

#define DIV(a, b) ((fixed_t)((((int64_t) a) << FP_SHIFT_AMOUNT) / b))

#endif /* threads/fixed_point.h */