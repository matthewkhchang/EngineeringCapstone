#include <stdio.h>
#include "esp_log.h"
#include "driver/i2c.h"

//Defines
#define SAMPLE_PERIOD_MS		200
#define I2C_MASTER_SCL_IO		4	             /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO		0	             /*!< gpio number for I2C master data  */
#define I2C_FREQ_HZ				100000           /*!< I2C master clock frequency */
#define I2C_TX_BUF_DISABLE  	0                /*!< I2C master do not need buffer */
#define I2C_RX_BUF_DISABLE  	0                /*!< I2C master do not need buffer */
#define ACK_CHECK_EN                       0x1
#define ACK_VAL                            0x0              /*!< I2C ack value */
#define NACK_VAL                           0x1              /*!< I2C nack value */

//Private Variables
static const char *TAG = "I2C Test";
static uint8_t aRxBuffer [2] = {0x00, 0x00};
static uint8_t aTxBuffer [2] = {0x00, 0x13};

//Public Function Declarations
void sensor_i2c_init(void);
uint16_t light_read(void);
uint16_t temp_read(void);

//Private Function Declarations
static void i2c_master_init();
static esp_err_t i2c_my_read(i2c_port_t i2c_num, uint8_t i2c_addr, uint8_t i2c_reg, uint8_t* data_rd, size_t size);
static esp_err_t i2c_my_write(i2c_port_t i2c_num, uint8_t i2c_addr, uint8_t i2c_reg, uint8_t* data_wr, size_t size);


//****************************************************************************
//Public Functions
//****************************************************************************

void sensor_i2c_init(void)
{
    int status;
    i2c_master_init();
    status = i2c_my_write(I2C_NUM_0, 0x10, 0x00, aTxBuffer, 2);
    ESP_LOGI(TAG,"Write Status: %d\n", status);

    aTxBuffer[1]=0x00;
    i2c_my_write(I2C_NUM_0, 0x10, 0x01, aTxBuffer, 2);
    i2c_my_write(I2C_NUM_0, 0x10, 0x02, aTxBuffer, 2);
    i2c_my_write(I2C_NUM_0, 0x10, 0x03, aTxBuffer, 2);
    
}


uint16_t light_read(void)
{
    static uint16_t data;
    i2c_my_read(I2C_NUM_0, 0x10, 0x04, aRxBuffer, 2);
    data = (aRxBuffer[1]<<8) | aRxBuffer[0];
    ESP_LOGI(TAG,"Light Reading: %d\n", data);
    return (data * 1.8432);
}


uint16_t temp_read(void)
{
    static uint16_t data;
    i2c_my_read(I2C_NUM_0, 0x50, 0x00, aRxBuffer, 2);
    data = ((aRxBuffer[0] & 0xF) <<4) | ((aRxBuffer[1] & 0xF0)>>4);
    ESP_LOGI(TAG,"Temp Reading: %d\n", data);
    return (30 - ((2560000/data - 18056)/443.7) );
}

//****************************************************************************
//Private Functions
//****************************************************************************

static void i2c_master_init(){
    int i2c_master_port = I2C_NUM_0;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,         // select GPIO specific to your project
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,         // select GPIO specific to your project
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,  // select frequency specific to your project
        // .clk_flags = 0,          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
    };
    i2c_param_config(i2c_master_port, &conf);
    i2c_driver_install(i2c_master_port, conf.mode, I2C_RX_BUF_DISABLE, I2C_TX_BUF_DISABLE, 0);
}

/**
 * @brief test code to read i2c slave device with registered interface
 * _______________________________________________________________________________________________________
 * | start | slave_addr + rd_bit +ack | register + ack | read n-1 bytes + ack | read 1 byte + nack | stop |
 * --------|--------------------------|----------------|----------------------|--------------------|------|
 *
 */
static esp_err_t i2c_my_read(i2c_port_t i2c_num, uint8_t i2c_addr, uint8_t i2c_reg, uint8_t* data_rd, size_t size){
    if (size == 0) {
        return ESP_OK;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();                                   //Step 1
    i2c_master_start(cmd);                                                          //Step 2
    i2c_master_write_byte(cmd, ( i2c_addr << 1 ), ACK_CHECK_EN);                    //Step 3 - Slave address, Write

    i2c_master_write_byte(cmd, i2c_reg, ACK_CHECK_EN);                              //       - Register address

    i2c_master_start(cmd);
 
    i2c_master_write_byte(cmd, ( i2c_addr << 1 ) | I2C_MASTER_READ, ACK_CHECK_EN);  //       - Slave address, Read

    if (size > 1) {
        i2c_master_read(cmd, data_rd, size - 1, ACK_VAL);                           //Step 4
    }
    i2c_master_read_byte(cmd, data_rd + size - 1, NACK_VAL);                        //Step 5

    i2c_master_stop(cmd);                                                           //Step 6

    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 0xffffffff);    //Step 7
    i2c_cmd_link_delete(cmd);                                                       //Step 8

    return ret;
}

/**
 * @brief Test code to write i2c slave device with registered interface
 *        Master device write data to slave(both esp32),
 *        the data will be stored in slave buffer.
 *        We can read them out from slave buffer.
 * ____________________________________________________________________________________
 * | start | slave_addr + wr_bit + ack | register + ack | write n bytes + ack  | stop |
 * --------|---------------------------|----------------|----------------------|------|
 *
 */
static esp_err_t i2c_my_write(i2c_port_t i2c_num, uint8_t i2c_addr, uint8_t i2c_reg, uint8_t* data_wr, size_t size){
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ( i2c_addr << 1 ) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, i2c_reg, ACK_CHECK_EN);
    i2c_master_write(cmd, data_wr, size, ACK_CHECK_EN);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 0xffffffff);
    i2c_cmd_link_delete(cmd);
    return ret;
}