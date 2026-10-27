import numpy as np
import matplotlib.pyplot as plt

name = "build/txdata.pcm"

# Мгновенное чтение всего файла
raw_data = np.fromfile(name, dtype=np.int16)

real_np = raw_data[0::2].astype(np.float64)
imag_np = raw_data[1::2].astype(np.float64)

min_len = min(len(real_np), len(imag_np))

# --- РУЧНАЯ ОБРЕЗКА ИМЕННО ЭТОГО УЧАСТКА ---
# Значения взяты с оси X твоего скриншота (от 11 млн до 11.4 млн)
start_idx = 11000000
end_idx = 11400000

# Защита, если файл окажется короче
start_idx = max(0, start_idx)
end_idx = min(min_len, end_idx)

real_np = real_np[start_idx:end_idx]
imag_np = imag_np[start_idx:end_idx]

# Расчет модуля только для вырезанного куска
magnitude = np.sqrt(real_np**2 + imag_np**2)
count = np.arange(start_idx, end_idx)

fig, axs = plt.subplots(1, 2, figsize=(14, 5))

# 1. График во времени (Только выбранная пачка)
axs[0].set_title("Сигнал во времени")
axs[0].plot(count, real_np, color='blue', label='I (Real)', alpha=0.8)
axs[0].plot(count, imag_np, color='red', label='Q (Imag)', alpha=0.8)
axs[0].plot(count, magnitude, color='green', linestyle='--', label='Модуль |z|', alpha=0.6)
axs[0].set_xlabel("Исходный номер сэмпла")
axs[0].set_ylabel("Амплитуда")
axs[0].grid(True)
axs[0].legend()

# 2. Диаграмма созвездия (Только для этого фрагмента)
axs[1].set_title("Диаграмма созвездия")
# Возвращаем прореживание (каждая 10-я точка) и делаем маркеры прозрачнее (alpha=0.1)
step = 10
axs[1].scatter(real_np[::step], imag_np[::step], color='purple', s=1, alpha=0.1)
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