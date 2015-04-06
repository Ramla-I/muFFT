#include "fft.h"
#include "fft_internal.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

struct mufft_step_1d
{
   mufft_1d_func func;
   unsigned radix;
   unsigned p;
};

struct mufft_plan_1d
{
   struct mufft_step_1d *steps;
   unsigned num_steps;

   cfloat *tmp_buffer;
   cfloat *twiddles;
};

static cfloat twiddle(int direction, int k, int p)
{
   double phase = (M_PI * direction * k) / p;
   return cos(phase) + I * sin(phase);
}

static cfloat *build_twiddles(unsigned N, int direction)
{
   cfloat *twiddles = mufft_alloc(N * sizeof(cfloat));
   if (twiddles == NULL)
   {
      return NULL;
   }

   cfloat *pt = twiddles;

   for (unsigned p = 1; p < N; p <<= 1)
   {
      for (unsigned k = 0; k < p; k++)
      {
         pt[k] = twiddle(direction, k, p);
      }
      pt += p == 2 ? 3 : p; // Make sure that twiddles for p == 4 and up are aligned properly for AVX.
   }

   return twiddles;
}

struct fft_step_1d
{
   mufft_1d_func func;
   unsigned minimum_elements;
   unsigned radix;
   unsigned fixed_p;
   unsigned minimum_p;
   unsigned flags;
};

struct fft_step_2d
{
   mufft_2d_func func;
   unsigned minimum_elements_x;
   unsigned minimum_elements_y;
   unsigned radix;
   unsigned fixed_p;
   unsigned minimum_p;
   unsigned flags;
};

static const struct fft_step_1d fft_1d_table[] = {
#define STAMP_CPU_1D(arch, ext, min_x) \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix8_p1_ ## ext, .minimum_elements = 8 * min_x, .radix = 8, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix4_p1_ ## ext, .minimum_elements = 4 * min_x, .radix = 4, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix2_p1_ ## ext, .minimum_elements = 2 * min_x, .radix = 2, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix2_p2_ ## ext, .minimum_elements = 2 * min_x, .radix = 2, .fixed_p = 2, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix8_generic_ ## ext, .minimum_elements = 8 * min_x, .radix = 8, .minimum_p = 8 }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix4_generic_ ## ext, .minimum_elements = 4 * min_x, .radix = 4, .minimum_p = 4 }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix2_generic_ ## ext, .minimum_elements = 2 * min_x, .radix = 2, .minimum_p = 4 }

   STAMP_CPU_1D(MUFFT_FLAG_CPU_AVX, avx, 4),
   STAMP_CPU_1D(MUFFT_FLAG_CPU_SSE3, sse3, 2),
   STAMP_CPU_1D(MUFFT_FLAG_CPU_SSE, sse, 2),
   STAMP_CPU_1D(0, c, 1),
};

static const struct fft_step_2d fft_2d_table[] = {
#define STAMP_CPU_2D(arch, ext, min_x) \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix8_p1_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 8, .radix = 8, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix4_p1_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 4, .radix = 4, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_FORWARD, \
      .func = mufft_forward_radix2_p1_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 2, .radix = 2, .fixed_p = 1, .minimum_p = -1u }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix8_generic_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 8, .radix = 8, .minimum_p = 8 }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix4_generic_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 4, .radix = 4, .minimum_p = 4 }, \
   { .flags = arch | MUFFT_FLAG_DIRECTION_ANY, \
      .func = mufft_radix2_generic_vert_ ## ext, .minimum_elements_x = min_x, .minimum_elements_y = 2, .radix = 2, .minimum_p = 4 }

   STAMP_CPU_2D(MUFFT_FLAG_CPU_AVX, avx, 4),
   STAMP_CPU_2D(MUFFT_FLAG_CPU_SSE3, sse3, 2),
   STAMP_CPU_2D(MUFFT_FLAG_CPU_SSE, sse, 2),
   STAMP_CPU_2D(0, c, 1),
};

static bool add_step_1d(mufft_plan_1d *plan, const struct fft_step_1d *step, unsigned p)
{
   struct mufft_step_1d *new_steps = realloc(plan->steps, (plan->num_steps + 1) * sizeof(*new_steps));
   if (new_steps != NULL)
   {
      plan->steps = new_steps;
      plan->steps[plan->num_steps] = (struct mufft_step_1d) {
         .func = step->func,
         .radix = step->radix,
         .p = p,
      };
      plan->num_steps++;
      return true;
   }
   else
   {
      return false;
   }
}

static bool build_plan_1d(mufft_plan_1d *plan, unsigned N, int direction)
{
   unsigned radix = N;
   unsigned p = 1;

   while (radix > 1)
   {
      bool found = false;

      // Find first (optimal?) routine which can do work.
      for (unsigned i = 0; i < ARRAY_SIZE(fft_1d_table); i++)
      {
         const struct fft_step_1d *step = &fft_1d_table[i];

         if (radix % step->radix == 0 &&
               N >= step->minimum_elements &&
               // TODO: Test for CPU flags!
               (p >= step->minimum_p || p == step->fixed_p))
         {
            if (add_step_1d(plan, step, p))
            {
               found = true;
               radix /= step->radix;
               p *= step->radix;
               break;
            }
         }
      }

      if (!found)
      {
         return false;
      }
   }

   return true;
}

mufft_plan_1d *mufft_create_plan_1d_c2c(unsigned N, int direction)
{
   if ((N & (N - 1)) != 0 || N == 1)
   {
      return NULL;
   }
   
   mufft_plan_1d *plan = mufft_calloc(sizeof(*plan));
   if (plan == NULL)
   {
      goto error;
   }

   plan->twiddles = build_twiddles(N, direction);
   if (plan->twiddles == NULL)
   {
      goto error;
   }

   plan->tmp_buffer = mufft_alloc(N * sizeof(cfloat));
   if (plan->tmp_buffer == NULL)
   {
      goto error;
   }

   if (!build_plan_1d(plan, N, direction))
   {
      goto error;
   }

   return plan;

error:
   mufft_free_plan_1d(plan);
   return NULL;
}

void mufft_execute_plan_1d(mufft_plan_1d *plan, void *output, const void *input)
{
}

void mufft_free_plan_1d(mufft_plan_1d *plan)
{
   if (plan == NULL)
   {
      return;
   }
   free(plan->steps);
   mufft_free(plan->tmp_buffer);
   mufft_free(plan->twiddles);
   mufft_free(plan);
}

void *mufft_alloc(size_t size)
{
   void *ptr;
   if (posix_memalign(&ptr, 64, size) < 0)
   {
      return NULL;
   }
   return ptr;
}

void *mufft_calloc(size_t size)
{
   void *ptr = mufft_alloc(size);
   if (ptr != NULL)
   {
      memset(ptr, 0, size);
   }
   return ptr;
}

void mufft_free(void *ptr)
{
   free(ptr);
}
