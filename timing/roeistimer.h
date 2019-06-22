#ifndef __TIMER_H__
#define __TIMER_H__

#define SAMPLES 500

extern "C" const char* roeis_func_names[];
extern "C" uint64_t roeis_times[];
extern "C" uint32_t roeis_funcs_len;

//inline void roeis_collect_time(const char* func_name);
//inline void roeis_report_time();
//inline void roeis_clear_time();
static inline uint32_t rdtscp() {
  uint32_t rv;
  asm volatile ("rdtscp": "=a" (rv) :: "edx", "ecx");
  return rv;
}

static inline uint64_t rdtscp64() {
  uint32_t low, high;
  asm volatile ("rdtscp": "=a" (low), "=d" (high) :: "ecx");
  return (((uint64_t)high) << 32) | low;
}

inline void roeis_collect_time(const char* func_name) {
	//commented-out to save some cycles
	//if (roeis_funcs_len > MAX_FUNCS) {
	//	return;
	//}
        //printf("collect %d\n",roeis_funcs_len);
  if(roeis_funcs_len == SAMPLES) 
    for(int i = 0; i < SAMPLES; i++) 
      fprintf(stderr, "%lu\n%s\n", roeis_times[i], roeis_func_names[i]);
  
  roeis_times[roeis_funcs_len] = rdtscp64();
  //printf("collect2 %d\n",roeis_funcs_len);
  //printf("%d: hello_time: %s\n", roeis_funcs_len, func_name);
  roeis_func_names[roeis_funcs_len] = func_name;
  roeis_funcs_len++;
}


inline void roeis_report_time() {
  if (roeis_funcs_len == 0) {
		return;
	}
	uint64_t prev_time = roeis_times[0];
	for (int i = 1; i < roeis_funcs_len; i++) {
                int64_t diff = roeis_times[i] - prev_time;
                if (diff < 0) {//underflow
			diff = prev_time - roeis_times[i];
		}
		//printf("%s --> %s %lu\n",roeis_func_names[i-1], roeis_func_names[i], diff);
		prev_time = roeis_times[i];
	}
}

inline void roeis_clear_time() {
        //printf("clean %d\n",roeis_funcs_len);
	roeis_funcs_len = 0;
}

#endif
