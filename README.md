<div align="center">
  <img src="./assets/header_banner.svg?raw=true" alt="ADALM-Pluto SDR Digital Communication System" width="100%" />

  <br />

  <img src="https://img.shields.io/badge/Platform-ADALM--PLUTO_(AD9361)-00599C?style=flat-square&logo=analogdevices&logoColor=white" alt="Platform" />
  <img src="https://img.shields.io/badge/Language-C11_%2F_C%2B%2B-00599C?style=flat-square&logo=c%2B%2B&logoColor=white" alt="C/C++" />
  <img src="https://img.shields.io/badge/API-SoapySDR_%7C_libiio-2E8B57?style=flat-square" alt="SoapySDR" />
  <img src="https://img.shields.io/badge/Analysis-Python_3_%7C_NumPy_%7C_Matplotlib-3776AB?style=flat-square&logo=python&logoColor=white" alt="Python" />
  <img src="https://img.shields.io/badge/Build-CMake-064F8C?style=flat-square&logo=cmake&logoColor=white" alt="CMake" />
</div>


## Программно-определяемый приемопередатчик на базе Adalm-Pluto SDR

Репозиторий содержит исходный код, скрипты спектрального и временного анализа, а также инженерные отчеты по разработке цифровой системы связи на базе программно-определяемого радио **Analog Devices ADALM-PLUTO (PlutoSDR)**.

Проект охватывает полный цикл проектирования физического уровня (PHY): от базового приема радиовещательных сигналов в GNU Radio до низкоуровневой реализации приемопередатчика на языке C/C++ с использованием аппаратных временных меток (Hardware Timestamps), формирования импульсов методом дискретной свертки, BPSK-модуляции и алгоритмов символьной синхронизации.

* **Разработчик:** Богдан
* **Аппаратная платформа:** 2x ADALM-Pluto SDR (трансивер AD9361, ЦАП/АЦП 12 бит)
* **Рабочий диапазон:** 800 МГц (ширина полосы пропускания 1 МГц)

---

<!-- ===================================================================== -->
<!-- БЛОК SUMMARY: ТАБЛИЦА ОТЧЕТОВ ПО НЕДЕЛЯМ                              -->
<!-- Чтобы добавить новую неделю, скопируй последнюю строку таблицы ниже   -->
<!-- ===================================================================== -->

## Сводная таблица отчетов (Summary)

Детальные отчеты с математическими выводами, графиками и разбором архитектуры распределены по папкам соответствующих недель.

| Недели | Тема лабораторной работы | Ключевые технологии и результаты | Статус | Папка с отчетом |
| :---: | :--- | :--- | :---: | :---: |
| **1–2** | GNU Radio. Построение FM-радиоприемника | Децимация, ФНЧ, демодуляция WBFM, анализ спектра на 101.1 МГц | ![Done](https://img.shields.io/badge/Сдано-2ea44f?style=flat-square) | [Week_1-2](Reports/Week_1-2/) |
| **3** | Инициализация ADALM-Pluto и аппаратные метки времени (Timestamp) | SoapySDR, `timestamp_every=1920`, проверка интервала 1.92 мс | ![Done](https://img.shields.io/badge/Сдано-2ea44f?style=flat-square) | [Week_3](Reports/Week_3/) |
| **4–6** | Работа с I/Q-буферами и радиоканал между платами | Формирование огибающей, синхронизация RX/TX, передача по эфиру | ![Done](https://img.shields.io/badge/Сдано-2ea44f?style=flat-square) | [Week_4-5-6](Reports/Week_4-5-6/) |
| **7** | Разделение ролей TX/RX и подготовка модуляции | Настройка `txStream` и `rxStream`, передача тестовых массивов | ![Done](https://img.shields.io/badge/Сдано-2ea44f?style=flat-square) | [Week_7](Reports/Week_7/) |
| **8** | Передатчик BPSK, дискретная свертка и анализ радиоканала | Upsampling, свертка, исследование CFO и утечки (LO Leakage) | ![Done](https://img.shields.io/badge/Сдано-2ea44f?style=flat-square) | [Week_8](Reports/Week_8/) |
| **9** | Приемник. Детерминированный кадр и выбор символьных отсчетов | Передача ASCII, генерация `message.h`, децимация по 10 фазам | ![Done](https://img.shields.io/badge/Готово-00599C?style=flat-square) | [Week_9](Reports/Week_9/) |
<!-- ===================================================================== -->
<!-- КОНЕЦ БЛОКА SUMMARY                                                   -->
<!-- ===================================================================== -->

---

## Архитектура системы

<p align="center">
  <img src="./assets/dsp_pipeline.svg?raw=true" alt="DSP Pipeline Architecture" width="100%" />
</p>

---

## Установка и сборка проекта

### 1. Клонирование репозитория и инициализация сабмодулей

Проект использует специализированные ветки библиотек с поддержкой аппаратных таймстемпов ADALM-Pluto, подключенные через механизм `git submodule`.

```bash
git clone --recurse-submodules https://github.com/<ваш-логин>/SDR.git
cd SDR
```

Если репозиторий уже был склонирован без флага `--recurse-submodules`, загрузите подмодули командой:

```bash
git submodule update --init --recursive
```

### 2. Установка системных зависимостей (Linux Mint / Ubuntu / Debian)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
    libusb-1.0-0-dev libxml2-dev bison flex libcdk5-dev \
    libaio-dev libavahi-common-dev libavahi-client-dev \
    python3 python3-numpy python3-matplotlib swig
```

### 3. Компиляция основного проекта

Сборка выполняется в изолированной директории `build/` (убедитесь, что эта директория не индексируется Git, она должна быть в `.gitignore`):

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

После успешной компиляции в директории `build/` появится исполняемый файл `main`.

---

## Инструкция по запуску

Проверка доступности подключенной платы ADALM-Pluto и определение ее URI выполняются утилитой:

```bash
iio_info -s
```

### Запуск в режиме приемника (RX)

На первом компьютере запускается приемник. Он инициализирует поток чтения, записывает принятые из эфира комплексные отсчеты в файл `build/txdata.pcm` и ожидает пакеты на протяжении 3000 буферов:

```bash
cd build
sudo ./main rx
```

### Запуск в режиме передатчика (TX)

На втором компьютере запускается передатчик. Программа формирует BPSK-сигнал из фиксированной текстовой строки `"Hello Pluto!"`, сохраняет эталонный массив сэмплов в заголовочный файл `message.h` и излучает каждый второй буфер в радиоэфир на частоте 800 МГц:

```bash
cd build
sudo ./main tx
```

### Визуализация и анализ данных (Python)

После завершения работы приемника запустите скрипт анализа из корневой директории проекта. Он автоматически найдет полезный сигнал в `txdata.pcm`, построит временные огибающие, созвездие и выполнит децимацию (сдвиги) для символьной синхронизации:

```bash
python3 readPcm.py
```
