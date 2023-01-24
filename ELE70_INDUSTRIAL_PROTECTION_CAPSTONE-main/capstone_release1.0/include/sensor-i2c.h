typedef struct {
	int id;
    int value;
} sensor_struct;

void sensor_i2c_init(void);
uint16_t light_read(void);
uint16_t temp_read(void);