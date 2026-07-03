path = r'F:\26年物联网大赛\AIOT\espnow-all\s3-receiver\espnow-receiver-s3\ble_manager.h'
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    lines = f.readlines()

# L317: {"name":"%s","o":%.1f,"h":%.1f,"c":%.1f,"v":%.1f,"co2":%u,
# L318: "t":%.1f,"hu":%.1f,
# 0-indexed: 316, 317

print(f'L317 before: {lines[316].rstrip()[:100]}')
print(f'L318 before: {lines[317].rstrip()[:100]}')

lines[316] = lines[316].replace('%.1f', '%.2f')
lines[317] = lines[317].replace('%.1f', '%.2f')

print(f'L317 after:  {lines[316].rstrip()[:100]}')
print(f'L318 after:  {lines[317].rstrip()[:100]}')

with open(path, 'w', encoding='utf-8') as f:
    f.writelines(lines)

print('\nOK: Sensor JSON precision changed to %.2f')
