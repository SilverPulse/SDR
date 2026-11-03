import numpy as np
import matplotlib.pyplot as plt

name = "build/txdata.pcm"

# Чтение данных
raw_data = np.fromfile(name, dtype=np.int16)
real_np = raw_data[0::2].astype(np.float64)
imag_np = raw_data[1::2].astype(np.float64)

min_len = min(len(real_np), len(imag_np))
real_np = real_np[:min_len]
imag_np = imag_np[:min_len]

# Расчет модуля
magnitude = np.sqrt(real_np**2 + imag_np**2)

# БЕРЕМ ВЕСЬ ФАЙЛ БЕЗ ОБРЕЗОК (Возвращаем масштаб "штрих-кода")
count_plot = np.arange(min_len)

fig, axs = plt.subplots(1, 2, figsize=(14, 5))

# 1. Сигнал во времени (Весь эфир)
axs[0].set_title("Сигнал во времени (Весь эфир)")
axs[0].plot(count_plot, real_np, color='blue', label='I (Real)', alpha=0.8, linewidth=0.5)
axs[0].plot(count_plot, imag_np, color='red', label='Q (Imag)', alpha=0.8, linewidth=0.5)
axs[0].plot(count_plot, magnitude, color='green', linestyle='--', label='Модуль |z|', alpha=0.6, linewidth=0.5)
axs[0].set_xlabel("Исходный номер сэмпла")
axs[0].set_ylabel("Амплитуда")
axs[0].grid(True)
axs[0].legend()

# 2. Созвездие (АБСОЛЮТНО ВСЕ СЫРЫЕ ТОЧКИ)
axs[1].set_title("Диаграмма созвездия (Все точки)")
# Никакого шага децимации, берем всё. Прозрачность 0.2 покажет, 
# где точки накладываются друг на друга (там цвет будет гуще).
axs[1].scatter(real_np, imag_np, color='purple', s=1, alpha=0.2)
axs[1].axhline(0, color='black', linewidth=0.8)
axs[1].axvline(0, color='black', linewidth=0.8)
axs[1].set_xlabel("In-Phase (I)")
axs[1].set_ylabel("Quadrature (Q)")
axs[1].grid(True)

max_val = max(np.max(np.abs(real_np)), np.max(np.abs(imag_np)))
limit = max_val * 1.2 if max_val != 0 else 1.0
axs[1].set_xlim([-limit, limit])
axs[1].set_ylim([-limit, limit])
axs[1].set_aspect('equal', 'box')

plt.tight_layout()
plt.show()