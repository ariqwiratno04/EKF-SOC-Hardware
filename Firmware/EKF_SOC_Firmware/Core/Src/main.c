/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "F7_INA226.h"
#include "usbd_cdc_if.h"
#include "arm_math.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define EKF_SAMPLE_TIME_S (0.1f)      // Waktu sampling 100ms = 0.1s
const uint16_t INA226_I2C_ADDRESS = 0x80;
const float INA226_BUS_VOLTAGE_LSB = 0.00125f;
const float INA226_CURRENT_LSB_MA = 0.5f;

// --- Parameter Model Baterai ---
#define BATTERY_CAPACITY_AH (28.0f)
#define R_TERMINAL (0.01f)
#define R_POLARIZATION (0.005f)
#define C_POLARIZATION (10000.0f)

// --- Inisialisasi State Awal ---
#define INITIAL_SOC (1.0f)      // Kondisi SOC awal (1.0 = 100%)
#define INITIAL_VP  (0.0f)      // Kondisi Vp awal (Volt)

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// --- Variabel dan Matriks EKF (dibuat static agar nilainya persisten) ---
static float32_t x_data[2] = {INITIAL_SOC, INITIAL_VP}; // State: [SOC; Vp]
static float32_t P_data[4] = {1e-4f, 0.0f, 0.0f, 1e-3f}; // Kovariansi Error State
static float32_t Q_data[4] = {1e-4f, 0.0f, 0.0f, 1e-3f}; // Kovariansi Noise Proses
static float32_t R_data[1] = {1e-2f}; // Kovariansi Noise Pengukuran

static arm_matrix_instance_f32 x; // Vektor State [SOC; Vp] (2x1)
static arm_matrix_instance_f32 P; // Matriks Kovariansi Error (2x2)
static arm_matrix_instance_f32 Q; // Matriks Noise Proses (2x2)
static arm_matrix_instance_f32 R; // Matriks Noise Pengukuran (1x1)

// --- Variabel untuk Model Baterai (Lookup Table) ---
static const float SOC_lookup[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
static const float Em_lookup[6]  = {2.775f, 3.0f, 3.7f, 3.8f, 4.0f, 4.3068f};
static const int lookup_size = 6;

// --- Variabel untuk Coulomb Counting ---
static float32_t soc_coulomb_counting = INITIAL_SOC;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void EKF_Init(void);
void EKF_Update(float Vt_meas, float I_meas);
float get_OCV_from_SOC(float soc);
float get_dOCV_dSOC(float soc);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */

  // Inisialisasi EKF
    EKF_Init();

    // Konfigurasi awal untuk INA226
    uint16_t config = INA226_AVG_16 | INA226_VBUS_588uS | INA226_VSH_588uS | INA226_MODE_CONT_SHUNT_AND_BUS;
    INA226_setConfig(&hi2c1, INA226_I2C_ADDRESS, config);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	  // 1. Baca data sensor dari INA226
	      float measured_voltage = (float)INA226_getBusVReg(&hi2c1, INA226_I2C_ADDRESS) * INA226_BUS_VOLTAGE_LSB;
	      // Arus dibaca dalam mA, lalu diubah ke A. Arus positif = discharge.
	      float measured_current_mA = (float)((int16_t)INA226_getCurrentReg(&hi2c1, INA226_I2C_ADDRESS)) * INA226_CURRENT_LSB_MA;
	      float measured_current_A = measured_current_mA / 1000.0f;

	      // 2. Jalankan satu siklus EKF
	      EKF_Update(measured_voltage, measured_current_A);

	      // 3. Hitung SoC menggunakan Coulomb Counting
	      float soc_delta = (measured_current_A * EKF_SAMPLE_TIME_S) / (BATTERY_CAPACITY_AH * 3600.0f);
	      soc_coulomb_counting -= soc_delta;
	      soc_coulomb_counting = fmaxf(0.0f, fminf(1.0f, soc_coulomb_counting)); // Clamping

	      // 4. Ambil hasil estimasi dari EKF
	      float estimated_soc_ekf = x_data[0] * 100.0f; // Ubah ke format persen
	      float estimated_soc_cc = soc_coulomb_counting * 100.0f;

	      // 5. Tampilkan hasil perbandingan melalui USB VCP
	      char tx_buffer[128];
	      sprintf(tx_buffer, "EKF:%.2f%% | CC:%.2f%% | V:%.3fV | I:%.3fA\r\n",
	              estimated_soc_ekf, estimated_soc_cc, measured_voltage, measured_current_A);
	      CDC_Transmit_FS((uint8_t*)tx_buffer, strlen(tx_buffer));

	      // Jeda 100ms sesuai waktu sampling
	      HAL_Delay(100);

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */


  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
  * @brief Menghitung OCV dari SOC menggunakan lookup table dengan interpolasi linear.
  * @note  Fungsi ini bisa diganti dengan model polinomial di masa depan.
  */
float get_OCV_from_SOC(float soc)
{
    if (soc <= SOC_lookup[0]) return Em_lookup[0];
    if (soc >= SOC_lookup[lookup_size - 1]) return Em_lookup[lookup_size - 1];

    for (int i = 0; i < lookup_size - 1; i++) {
        if (soc >= SOC_lookup[i] && soc < SOC_lookup[i+1]) {
            return Em_lookup[i] + (soc - SOC_lookup[i]) * (Em_lookup[i+1] - Em_lookup[i]) / (SOC_lookup[i+1] - SOC_lookup[i]);
        }
    }
    return Em_lookup[lookup_size - 1]; // Fallback jika tidak ada segmen yang cocok
}

/**
  * @brief Menghitung turunan dOCV/dSOC (gradien) dari kurva OCV-SOC.
  * @note  Fungsi ini bisa diganti dengan turunan dari model polinomial.
  */
float get_dOCV_dSOC(float soc)
{
    float delta_soc = 0.0f, delta_em = 0.0f;

    if (soc <= SOC_lookup[0]) {
        delta_soc = SOC_lookup[1] - SOC_lookup[0];
        delta_em = Em_lookup[1] - Em_lookup[0];
    } else if (soc >= SOC_lookup[lookup_size - 1]) {
        delta_soc = SOC_lookup[lookup_size - 1] - SOC_lookup[lookup_size - 2];
        delta_em = Em_lookup[lookup_size - 1] - Em_lookup[lookup_size - 2];
    } else {
        for (int i = 0; i < lookup_size - 1; i++) {
            if (soc >= SOC_lookup[i] && soc < SOC_lookup[i+1]) {
                delta_soc = SOC_lookup[i+1] - SOC_lookup[i];
                delta_em = Em_lookup[i+1] - Em_lookup[i];
                break;
            }
        }
    }

    if (fabsf(delta_soc) < 1e-9f) { // Hindari pembagian dengan nol
        return 0.0f;
    }
    return delta_em / delta_soc;
}

/**
  * @brief Inisialisasi matriks EKF.
  */
void EKF_Init(void)
{
    // Hubungkan data array dengan struktur instance matriks CMSIS-DSP
    arm_mat_init_f32(&x, 2, 1, x_data);
    arm_mat_init_f32(&P, 2, 2, P_data);
    arm_mat_init_f32(&Q, 2, 2, Q_data);
    arm_mat_init_f32(&R, 1, 1, R_data);
}

/**
  * @brief Menjalankan satu langkah EKF (Prediksi dan Koreksi).
  */
void EKF_Update(float Vt_meas, float I_meas)
{
    // --- Deklarasi Matriks Lokal & Variabel ---
    float32_t A_data[4], B_data[2], P_pred_data[4], H_data[2], K_data[2];
    float32_t temp_mat_2x2_1_data[4], temp_mat_2x2_2_data[4];
    float32_t temp_vec_2x1_1_data[2];
    float32_t S_k_data[1], S_k_inv_data[1], H_T_data[2];

    arm_matrix_instance_f32 A, B, P_pred, H, K, H_T;
    arm_matrix_instance_f32 temp_mat1, temp_mat2, temp_vec1;
    arm_matrix_instance_f32 S_k, S_k_inv;

    // Inisialisasi semua instance matriks lokal
    arm_mat_init_f32(&A, 2, 2, A_data);
    arm_mat_init_f32(&B, 2, 1, B_data);
    arm_mat_init_f32(&P_pred, 2, 2, P_pred_data);
    arm_mat_init_f32(&H, 1, 2, H_data);
    arm_mat_init_f32(&K, 2, 1, K_data);
    arm_mat_init_f32(&H_T, 2, 1, H_T_data);
    arm_mat_init_f32(&temp_mat1, 2, 2, temp_mat_2x2_1_data);
    arm_mat_init_f32(&temp_mat2, 2, 2, temp_mat_2x2_2_data);
    arm_mat_init_f32(&temp_vec1, 2, 1, temp_vec_2x1_1_data);
    arm_mat_init_f32(&S_k, 1, 1, S_k_data);
    arm_mat_init_f32(&S_k_inv, 1, 1, S_k_inv_data);

    // --- Langkah 1: PREDIKSI STATE ---
    float alpha = expf(-EKF_SAMPLE_TIME_S / (R_POLARIZATION * C_POLARIZATION));
    A_data[0] = 1.0f; A_data[1] = 0.0f;
    A_data[2] = 0.0f; A_data[3] = alpha;

    B_data[0] = -EKF_SAMPLE_TIME_S / (BATTERY_CAPACITY_AH * 3600.0f);
    B_data[1] = R_POLARIZATION * (1.0f - alpha);

    float32_t B_I_data[2];
    arm_matrix_instance_f32 B_I;
    arm_mat_init_f32(&B_I, 2, 1, B_I_data);
    arm_scale_f32(B_data, I_meas, B_I_data, 2); // B_I = B * I_meas

    float32_t x_pred_data[2];
    arm_matrix_instance_f32 x_pred;
    arm_mat_init_f32(&x_pred, 2, 1, x_pred_data);

    arm_mat_mult_f32(&A, &x, &temp_vec1);        // temp_vec1 = A * x
    arm_mat_add_f32(&temp_vec1, &B_I, &x_pred);  // x_pred = A*x + B*I

    x_pred_data[0] = fmaxf(0.0f, fminf(1.0f, x_pred_data[0])); // Clamp SOC [0,1]

    // --- Langkah 2: PREDIKSI KOVARIANS ERROR ---
    // P_pred = A * P * A' + Q
    arm_mat_trans_f32(&A, &H_T); // Gunakan H_T sementara untuk A transpose
    arm_mat_mult_f32(&A, &P, &temp_mat1);
    arm_mat_mult_f32(&temp_mat1, &H_T, &P_pred);
    arm_mat_add_f32(&P_pred, &Q, &P_pred);

    // --- Langkah 3: PREDIKSI PENGUKURAN ---
    float soc_pred = x_pred_data[0];
    float vp_pred = x_pred_data[1];
    float voc_pred = get_OCV_from_SOC(soc_pred);
    // Arus positif = discharge, sehingga mengurangi tegangan terminal
    float vt_pred = voc_pred - vp_pred - (R_TERMINAL * I_meas);

    // --- Langkah 4: HITUNG JACOBIAN (H) ---
    H_data[0] = get_dOCV_dSOC(soc_pred);
    H_data[1] = -1.0f;

    // --- Langkah 5: HITUNG INOVASI ---
    float e_innov = Vt_meas - vt_pred;

    // --- Langkah 6: HITUNG KOVARIANS INOVASI ---
    // S_k = H * P_pred * H' + R
    arm_mat_trans_f32(&H, &H_T);
    arm_mat_mult_f32(&H, &P_pred, &temp_vec1); // temp_vec1 (1x2)
    arm_mat_mult_f32(&temp_vec1, &H_T, &S_k);
    arm_mat_add_f32(&S_k, &R, &S_k);

    // --- Langkah 7: HITUNG KALMAN GAIN ---
    // K = P_pred * H' * inv(S_k)
    if (fabsf(S_k_data[0]) > 1e-9f) {
        arm_mat_inverse_f32(&S_k, &S_k_inv); // Invers dari matriks 1x1
        arm_mat_mult_f32(&P_pred, &H_T, &temp_vec1); // temp_vec1(2x1)
        arm_scale_f32(temp_vec1.pData, S_k_inv_data[0], K.pData, 2); // K = temp_vec1 * S_k_inv
    } else {
        K_data[0] = 0.0f; K_data[1] = 0.0f; // Reset gain jika S_k singular
    }

    // --- Langkah 8: UPDATE STATE ---
    // x = x_pred + K * e_innov
    arm_scale_f32(K.pData, e_innov, temp_vec1.pData, 2);
    arm_mat_add_f32(&x_pred, &temp_vec1, &x);

    x_data[0] = fmaxf(0.0f, fminf(1.0f, x_data[0])); // Clamp corrected SOC

    // --- Langkah 9: UPDATE KOVARIANS ERROR ---
    // P = (I - K * H) * P_pred
    arm_matrix_instance_f32 I; // Matriks Identitas 2x2
    float32_t I_data[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    arm_mat_init_f32(&I, 2, 2, I_data);

    arm_mat_mult_f32(&K, &H, &temp_mat1);
    arm_mat_sub_f32(&I, &temp_mat1, &temp_mat2);
    arm_mat_mult_f32(&temp_mat2, &P_pred, &P);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
