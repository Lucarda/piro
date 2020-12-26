
/*
 *  partition convolve
 *
 *  partition convolve performs FFT-based partitioned convolution.
 *	
 *  Typically partition_convolve~ might be used in conjuction with time_domain_convolve for zero-latency convolution with longer impulses.
 *
 *  Copyright 2012 Alex Harker. All rights reserved.
 *
 */

#include "u_partition_convolve.h"
#include <time.h>


/* Pointer Utility Macro (cache the offset calculation explictly if we are passing in an expression) */

#define DSP_SPLIT_COMPLEX_POINTER_CALC(complex1, complex2, offset)	\
  {									\
    t_uint temp_offset = offset;					\
    complex1.realp = complex2.realp + temp_offset;			\
    complex1.imagp = complex2.imagp + temp_offset;			\
  }


/* Random seeding for rand */

static unsigned int get_rand_seed()
{
  unsigned int seed;

#if defined (UNIX) || defined (MACOSX)
  seed = arc4random();
#else
  HCRYPTPROV hProvider = 0;
  const DWORD dwLength = 4;
  BYTE *pbBuffer = (BYTE *) &seed;
    
  if (!CryptAcquireContextW(&hProvider, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
    return 0;
    
  CryptGenRandom(hProvider, dwLength, pbBuffer);
  CryptReleaseContext(hProvider, 0);
#endif
	
  return seed;
}


t_uint partition_convolve_int_log2(t_uint in, t_uint *inexact)
{
  t_uint temp = in;
  t_uint out = 0;

  if (!in)
    return 0;
	
  while (temp)
    {
      temp >>= 1;
      out++;		
    }
	
  if (in == (t_uint) 1 << (out - 1)) 
    {
      out--;
      *inexact = 0;
    }
  else
    *inexact = 1;
	
  return out;
}


void init_partition_convolve()
{
  srand(get_rand_seed());
}


void partition_convolve_free(t_partition_convolve *x)
{
  if (!x)
    return;

  t_uint max_impulse_length = x->max_impulse_length;
  t_uint max_fft_over_4 = (1 << x->max_fft_size_log2) >> 2;
	
  destroy_setup32(x->fft_setup_real);
  aligned_freebytes(x->impulse_buffer.realp, max_impulse_length * 4 * sizeof(t_float32));
  aligned_freebytes(x->fft_buffers[0], (max_fft_over_4 * 6 * sizeof(vFloat)));
  freebytes(x, sizeof(t_partition_convolve));
}


t_convolve_error partition_convolve_max_fft_size_set(t_partition_convolve *x, t_uint max_fft_size)
{
  t_uint inexact = 0;
  t_uint max_fft_size_log2 = partition_convolve_int_log2(max_fft_size, &inexact);
	
  t_convolve_error error = CONVOLVE_ERR_NONE;
	
  if (max_fft_size_log2 > MAX_FFT_SIZE_LOG2) {
    error = CONVOLVE_ERR_FFT_SIZE_MAX_TOO_LARGE;
    max_fft_size_log2 = MAX_FFT_SIZE_LOG2;
  }
  
  if (max_fft_size_log2 && max_fft_size_log2 < MIN_FFT_SIZE_LOG2) {
    error = CONVOLVE_ERR_FFT_SIZE_MAX_TOO_SMALL;
    max_fft_size_log2 = MIN_FFT_SIZE_LOG2;
  }
	
  if(inexact)
    error = CONVOLVE_ERR_FFT_SIZE_MAX_NON_POWER_OF_TWO;
	
  x->max_fft_size_log2 = max_fft_size_log2;
  x->max_fft_size = (t_uint) 1 << max_fft_size_log2;
	
  return error;
}


t_partition_convolve *partition_convolve_new(t_uint max_fft_size, t_uint max_impulse_length, t_uint offset, t_uint length)
{
  t_partition_convolve *x = (t_partition_convolve *)getbytes(sizeof(t_partition_convolve));
  t_uint max_fft_over_4;
	
  if(!x)
    return 0;
	
  /* Set default initial attributes and variables */
	
  x->num_partitions = 0;
  x->fft_size_log2 = 0;
  x->fft_size = 0;
	
  x->max_impulse_length = max_impulse_length;
  partition_convolve_max_fft_size_set(x, max_fft_size);
  partition_convolve_fft_size_set(x, x->max_fft_size);
  partition_convolve_offset_set(x, offset);
  partition_convolve_length_set(x, length);
	
  /* Allocate impulse buffer and input buffer */
	
  max_impulse_length = x->max_impulse_length;	
  max_fft_over_4 = (1 << x->max_fft_size_log2) >> 2;
	
  /* This is designed to make sure we can load the max impulse length, whatever the fft size */
	
  if(max_impulse_length % (max_fft_over_4 << 1)) {
    max_impulse_length /= (max_fft_over_4 << 1);
    max_impulse_length++;
    max_impulse_length *= (max_fft_over_4 << 1);
  }
	
  x->impulse_buffer.realp = (t_float32 *)aligned_getbytes((max_impulse_length * 4 * sizeof(t_float32)));
  x->impulse_buffer.imagp = x->impulse_buffer.realp + max_impulse_length;
  x->input_buffer.realp = x->impulse_buffer.imagp + max_impulse_length;
  x->input_buffer.imagp = x->input_buffer.realp + max_impulse_length;
	
  /* Allocate fft and temporary buffers	*/
	
  x->fft_buffers[0] = (vFloat *)aligned_getbytes((max_fft_over_4 * 6 * sizeof(vFloat)));
  x->fft_buffers[1] = x->fft_buffers[0] + max_fft_over_4;
  x->fft_buffers[2] = x->fft_buffers[1] + max_fft_over_4;
  x->fft_buffers[3] = x->fft_buffers[2] + max_fft_over_4;
	
  x->accum_buffer.realp = (t_float32 *)(x->fft_buffers[3] + max_fft_over_4);
  x->accum_buffer.imagp = x->accum_buffer.realp + (max_fft_over_4 * 2);
  x->partition_temp.realp = x->accum_buffer.imagp + (max_fft_over_4 * 2);
  x->partition_temp.imagp = x->partition_temp.realp + (max_fft_over_4 * 2);
		
  x->fft_setup_real = create_setup32(x->max_fft_size_log2);
		
  if(!x->impulse_buffer.realp || !x->fft_buffers[0] || !x->fft_setup_real) {
    partition_convolve_free(x);
    return 0;
  }
	
  return (x);
}


t_convolve_error partition_convolve_fft_size_set(t_partition_convolve *x, t_uint fft_size)
{
  t_uint inexact = 0;
  t_uint fft_size_log2 = partition_convolve_int_log2(fft_size, &inexact);
	
  t_convolve_error error = CONVOLVE_ERR_NONE;
	
  if (fft_size_log2 < MIN_FFT_SIZE_LOG2 || fft_size_log2 > x->max_fft_size_log2)
    return CONVOLVE_ERR_FFT_SIZE_OUT_OF_RANGE;
	
  if (inexact)
    error = CONVOLVE_ERR_FFT_SIZE_NON_POWER_OF_TWO;
	
  /* Set fft variables iff the fft size has actually actually changed */
	
  if (fft_size_log2 != x->fft_size_log2) {
    x->num_partitions = 0;
		
    /* Initialise fft info */
		
    fft_size = (t_uint) 1 << fft_size_log2;
    x->fft_size_log2 = fft_size_log2;
    x->fft_size = fft_size;
  }
	
  return error;
}


t_convolve_error partition_convolve_length_set(t_partition_convolve *x, t_uint length)
{
  t_uint max_impulse_length = x->max_impulse_length;
	
  t_convolve_error error = CONVOLVE_ERR_NONE;
	
  if (length > max_impulse_length) {
    error = CONVOLVE_ERR_PARTITION_LENGTH_TOO_LARGE;
    length = max_impulse_length;
  }
	
  x->length = length;
	
  return error;
}


void partition_convolve_offset_set(t_partition_convolve *x, t_uint offset)
{
  x->offset = offset;
}


t_convolve_error partition_convolve_set(t_partition_convolve *x, t_float *input, t_uint impulse_length)
{	
  t_convolve_error error = CONVOLVE_ERR_NONE;
	
  /* FFT variables / attributes */
	
  FFT_Setup32 *fft_setup_real = x->fft_setup_real;
	
  t_uint buffer_pos;
  t_uint fft_size = x->fft_size;
  t_uint fft_size_halved = fft_size >> 1;
  t_uint fft_size_log2 = x->fft_size_log2;
	
  /* Partition to load */
	
  t_uint length = x->length;
  t_uint offset = x->offset;
	
  /* Partition variables */
	
  t_float32 *buffer_temp1 = x->partition_temp.realp; /* */
  
  FFT_Split32 impulse_buffer = x->impulse_buffer;
  FFT_Split32 buffer_temp2;
	
  t_uint num_partitions;
  t_uint n_samps;
  t_uint i = 0;
	
  /* Calculate how much of the buffer to load */
	
  if (!input || impulse_length <= offset)
    impulse_length = 0;
  else
    impulse_length -= offset;

  if (length && length < impulse_length)
    impulse_length = length;

  if (impulse_length > x->max_impulse_length) {
    impulse_length = x->max_impulse_length;
    error = CONVOLVE_ERR_MEM_ALLOC_TOO_SMALL;
  }
	
  /* Partition / load the impulse */
	
  for(buffer_pos = offset, buffer_temp2 = impulse_buffer, num_partitions = 0; impulse_length > 0; buffer_pos += fft_size_halved, num_partitions++) {
    
    /* Get samples up to half the fft size */
			
    n_samps = (impulse_length > fft_size_halved) ? fft_size_halved : impulse_length;			
    impulse_length -= n_samps;
		
    /* Get samples */

    /* post("n_samps = %d fft_size = %d", n_samps, fft_size); */
    
    for (i = 0; i < n_samps; i++)
      buffer_temp1[i] = (t_float32)input[buffer_pos + i];

								 
    /* Zero pad */
		
    for (i = n_samps; i < fft_size; i++)
      buffer_temp1[i] = 0;
			
    /* Do fft straight into position */
		
    unzip_complex32(buffer_temp1, &buffer_temp2, (t_uint) 1 << (fft_size_log2 - (t_uint)1));
    do_real_fft32(&buffer_temp2, fft_setup_real, fft_size_log2);
    DSP_SPLIT_COMPLEX_POINTER_CALC(buffer_temp2, buffer_temp2, fft_size_halved);
  }
	
  x->reset_flag = 1;
  x->num_partitions = num_partitions;
	
  return error;
}		


t_bool partition_convolve_process(t_partition_convolve *x, vFloat *in, vFloat *out, t_uint vec_size)
{		
  FFT_Split32 impulse_buffer = x->impulse_buffer;
  FFT_Split32 input_buffer = x->input_buffer;
  FFT_Split32 accum_buffer = x->accum_buffer;
  FFT_Split32 impulse_temp, buffer_temp;	
	
  /* Scheduling variables */
	
  t_uint num_partitions = x->num_partitions;
  t_uint valid_partitions = x->valid_partitions;
  t_uint partitions_done = x->partitions_done;
  t_uint last_partition = x->last_partition;
  t_uint next_partition;
	
  t_int num_partitions_to_do;
	
  t_uint input_position = x->input_position;
  t_uint schedule_counter = x->schedule_counter;
	
  /* FFT variables */
	
  FFT_Setup32 *fft_setup_real = x->fft_setup_real;
	
  vFloat **fft_buffers = x->fft_buffers;
  vFloat *temp_vpointer1;
  /* vFloat *temp_vpointer2; */
	
  t_uint fft_size = x->fft_size;
  t_uint fft_size_halved = fft_size >> 1 ;
  t_uint fft_size_over_4 = fft_size >> 2;
  t_uint fft_size_halved_over_4 = fft_size_halved >> 2;
  t_uint fft_size_log2 = x->fft_size_log2;
	
  t_uint till_next_fft = x->till_next_fft;
  t_uint rw_pointer1 = x->rw_pointer1;
  t_uint rw_pointer2 = x->rw_pointer2;
	
  t_uint vec_remain = vec_size >> 2;
  t_uint loop_size;
  t_uint i;

  t_int random_fft_offset;
  char reset_flag = x->reset_flag;
	
  vFloat vscale_mult = float2vector((t_float32) (1.0 / (double) (fft_size << 2)));
  vFloat Zero = {0.f, 0.f, 0.f, 0.f};
	
  if(!num_partitions)
    return false;
	
  /* If we need to reset everything we do that here - happens when the fft size changes, or a new buffer is loaded */
	
  if(reset_flag) {
    /* Reset fft buffers + accum buffer */
		
    for (i = 0; i < (x->max_fft_size >> 2) * 5; i++)
      fft_buffers[0][i] = Zero;
		
    /* Reset fft offset (randomly) */
		
    while (fft_size_halved_over_4 <= (t_uint) (random_fft_offset = rand() / (RAND_MAX / fft_size_halved_over_4)));
		
    till_next_fft = random_fft_offset;
    rw_pointer1 = (fft_size_halved_over_4) - till_next_fft;
    rw_pointer2 = rw_pointer1 + (fft_size_halved_over_4);
		
    /* Reset scheduling variables */
		
    input_position = 0;
    schedule_counter = 0;
    partitions_done = 0;
    last_partition = 0;
    valid_partitions = 1;
		
    /* Set reset flag off */
		
    x->reset_flag = 0;
  }
	
  /* Main loop */
	
  while (vec_remain > 0) {
    
    /* How many vSamples to deal with this loop (depending on whether there is an fft to do before the end of the signal vector) */
		
    loop_size = vec_remain < till_next_fft ? vec_remain : till_next_fft;
    till_next_fft -= loop_size;
    vec_remain -= loop_size;
		
    /* Load input into buffer (twice) and output from the output buffer */
		
    for (i = 0; i < loop_size; i++) {
      *(fft_buffers[0] + rw_pointer1) = *in; 
      *(fft_buffers[1] + rw_pointer2) = *in;

      
      *out++ = *(fft_buffers[3] + rw_pointer1);	
			
      rw_pointer1++;
      rw_pointer2++;
      in++;
    }
		
    /* Work loop and scheduling - this is where most of the convolution is done */
    /* How many partitions to do this vector (make sure that all partitions are done before we need to do the next fft)? */
		
    if (++schedule_counter >= (fft_size_halved / vec_size) - 1) 
      num_partitions_to_do = (valid_partitions - partitions_done) - 1;
    else
      num_partitions_to_do = ((schedule_counter * (valid_partitions - 1)) / ((fft_size_halved / vec_size) - 1)) - partitions_done;
		
    while (num_partitions_to_do > 0) {
      /* Calculate buffer wraparounds (if wraparound is in the middle of this set of partitions this loop will run again) */
			
      next_partition = (last_partition < num_partitions) ? last_partition : 0;
      last_partition = (next_partition + num_partitions_to_do) > num_partitions ? num_partitions : next_partition + num_partitions_to_do;
      num_partitions_to_do -= last_partition - next_partition;
			
      /* Calculate offsets and pointers */
			
      DSP_SPLIT_COMPLEX_POINTER_CALC(impulse_temp, impulse_buffer, ((partitions_done + 1) * fft_size_halved));
      DSP_SPLIT_COMPLEX_POINTER_CALC(buffer_temp, input_buffer, (next_partition * fft_size_halved));
			
      /* Do processing */
			
      for (i = next_partition; i < last_partition; i++) {	
	partition_convolve_process_partition(buffer_temp, impulse_temp, accum_buffer, fft_size_halved_over_4); /* */
	DSP_SPLIT_COMPLEX_POINTER_CALC(impulse_temp, impulse_temp, fft_size_halved);
	DSP_SPLIT_COMPLEX_POINTER_CALC(buffer_temp, buffer_temp, fft_size_halved);
	partitions_done++;
      }
    }
		
    /* FFT processing - this is where we deal with the fft, any windowing, the first partition and overlapping	*/
    /* First check that there is a new FFTs worth of buffer */
		
    if (till_next_fft == 0) {			
      /* Calculate the position to do the fft from/ to and calculate relevant pointers */
			
      temp_vpointer1 = (rw_pointer1 == fft_size_over_4) ? fft_buffers[1] : fft_buffers[0];
      DSP_SPLIT_COMPLEX_POINTER_CALC(buffer_temp, input_buffer, (input_position * fft_size_halved));
						
      /* Do the fft and put into the input buffer */
			
      unzip_complex32((t_float32 *)temp_vpointer1, &buffer_temp, (t_uint) 1 << (fft_size_log2 - (t_uint)1));

      /* */
      
      do_real_fft32(&buffer_temp, fft_setup_real, fft_size_log2);
			
      /* Process first partition here and accumulate the output (we need it now!) */
			
      partition_convolve_process_partition(buffer_temp, impulse_buffer, accum_buffer, fft_size_halved_over_4); /* */

      /* post("real = %f imag = %f", buffer_temp.realp[i], buffer_temp.imagp[i]); */
			
      /* Processing done - do inverse fft on the accumulation buffer */
			
      do_real_ifft32(&accum_buffer, fft_setup_real, fft_size_log2);
      zip_sample32(&accum_buffer, (t_float32 *)fft_buffers[2], (t_uint) 1 << (fft_size_log2 - (t_uint) 1));
			
      /* Calculate temporary output pointers */
			
      if (rw_pointer1 == fft_size_over_4) {
	temp_vpointer1 = fft_buffers[3];					
	/* temp_vpointer2 = fft_buffers[3] + fft_size_halved_over_4;    */
      }
      else {
	temp_vpointer1 = fft_buffers[3] + fft_size_halved_over_4;   
	/* temp_vpointer2 = fft_buffers[3]; */
      }
			
      /* Scale and store into output buffer (overlap-save) */
			
      for (i = 0; i < fft_size_halved_over_4; i++)
	*(temp_vpointer1++) = F32_VEC_MUL_OP(*(fft_buffers[2] + i), vscale_mult);
			
      /* Clear accumulation buffer */
			
      for (i = 0; i < fft_size_halved; i++)
	accum_buffer.realp[i] = 0;
      for (i = 0; i < fft_size_halved; i++)
	accum_buffer.imagp[i] = 0;
			
      /* Reset rw_pointers */
			
      if (rw_pointer1 == fft_size_over_4) 
	rw_pointer1 = 0;
      else 
	rw_pointer2 = 0;
      
      /* Set fft variables */
			
      till_next_fft = fft_size_halved_over_4;
			
      /* Set scheduling variables */
			
      if (++valid_partitions > num_partitions) 
	valid_partitions = num_partitions;
      
      if (input_position-- == 0)
	input_position = num_partitions - 1;
      
      last_partition = input_position + 1;
      schedule_counter = 0;
      partitions_done = 0;
    }
  }
	
  /* Write all variables back into the object struct */
	
  x->input_position = input_position;
  x->till_next_fft = till_next_fft;
  x->rw_pointer1 = rw_pointer1;
  x->rw_pointer2 = rw_pointer2;
	
  x->schedule_counter = schedule_counter;
  x->valid_partitions = valid_partitions;
  x->partitions_done = partitions_done;
  x->last_partition = last_partition;
	
  return true;
}


void partition_convolve_process_partition(FFT_Split32 in1, FFT_Split32 in2, FFT_Split32 out, t_uint num_vecs)
{
  vFloat *in_real1 = (vFloat *) in1.realp;
  vFloat *in_imag1 = (vFloat *) in1.imagp;
  vFloat *in_real2 = (vFloat *) in2.realp;
  vFloat *in_imag2 = (vFloat *) in2.imagp;
  vFloat *out_real = (vFloat *) out.realp;
  vFloat *out_imag = (vFloat *) out.imagp;
	
  t_float32 nyquist1, nyquist2;
  t_uint i;
	
  /*	Do Nyquist Calculation and then zero these bins */
	
  nyquist1 = in1.imagp[0];
  nyquist2 = in2.imagp[0];
	
  out.imagp[0] += nyquist1 * nyquist2;

  in1.imagp[0] = 0.f;
  in2.imagp[0] = 0.f;
	
  /* Do other bins (loop unrolled) */
	
  for (i = 0; i + 3 < num_vecs; i += 4) {
      out_real[i+0] = F32_VEC_ADD_OP(out_real[i+0], F32_VEC_SUB_OP(F32_VEC_MUL_OP(in_real1[i+0], in_real2[i+0]), F32_VEC_MUL_OP(in_imag1[i+0], in_imag2[i+0])));
      
      out_imag[i+0] = F32_VEC_ADD_OP(out_imag[i+0], F32_VEC_ADD_OP(F32_VEC_MUL_OP(in_real1[i+0], in_imag2[i+0]), F32_VEC_MUL_OP(in_imag1[i+0], in_real2[i+0])));
      
      out_real[i+1] = F32_VEC_ADD_OP(out_real[i+1], F32_VEC_SUB_OP(F32_VEC_MUL_OP(in_real1[i+1], in_real2[i+1]), F32_VEC_MUL_OP(in_imag1[i+1], in_imag2[i+1])));
      
      out_imag[i+1] = F32_VEC_ADD_OP(out_imag[i+1], F32_VEC_ADD_OP(F32_VEC_MUL_OP(in_real1[i+1], in_imag2[i+1]), F32_VEC_MUL_OP(in_imag1[i+1], in_real2[i+1])));
      
      out_real[i+2] = F32_VEC_ADD_OP(out_real[i+2], F32_VEC_SUB_OP(F32_VEC_MUL_OP(in_real1[i+2], in_real2[i+2]), F32_VEC_MUL_OP(in_imag1[i+2], in_imag2[i+2])));
      
      out_imag[i+2] = F32_VEC_ADD_OP(out_imag[i+2], F32_VEC_ADD_OP(F32_VEC_MUL_OP(in_real1[i+2], in_imag2[i+2]), F32_VEC_MUL_OP(in_imag1[i+2], in_real2[i+2])));
      
      out_real[i+3] = F32_VEC_ADD_OP(out_real[i+3], F32_VEC_SUB_OP(F32_VEC_MUL_OP(in_real1[i+3], in_real2[i+3]), F32_VEC_MUL_OP(in_imag1[i+3], in_imag2[i+3])));
      
      out_imag[i+3] = F32_VEC_ADD_OP(out_imag[i+3], F32_VEC_ADD_OP(F32_VEC_MUL_OP(in_real1[i+3], in_imag2[i+3]), F32_VEC_MUL_OP(in_imag1[i+3], in_real2[i+3])));
      
    }

  /* Replace nyquist bins */
	
  in1.imagp[0] = nyquist1;
  in2.imagp[0] = nyquist2;
}
 
