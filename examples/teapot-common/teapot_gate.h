/* Shared TEAPOT_GATE protocol and small helpers for both teapot clients. */
int teapot_gate_enabled(void);
void teapot_gate_hello(void);
void teapot_gate_phase(const char *name);
void teapot_gate_size(const char *phase, int width, int height);
void teapot_gate_maps(void);
void teapot_gate_pass(void);
void teapot_result(const char *name, int ok, const char *detail);
double teapot_now(void);
int teapot_env_flag(const char *name);
int teapot_env_int(const char *name, int fallback);
int teapot_software_renderer(const char *renderer);
void teapot_body_color(float *r, float *g, float *b);
