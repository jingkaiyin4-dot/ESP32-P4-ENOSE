path = r'F:\26年物联网大赛\AIOT\espnow-all\s3-receiver\espnow-receiver-s3\ble_manager.h'
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    content = f.read()

# Change %.1f to %.2f for sensor values in JSON
old = '"o":%.1f,"h":%.1f,"c":%.1f,"v":%.1f,"co2":%u,'
new = '"o":%.2f,"h":%.2f,"c":%.2f,"v":%.2f,"co2":%u,'
if old in content:
    content = content.replace(old, new)
    print('OK: o/h/c/v changed to %.2f')
else:
    print('SKIP: o/h/c/v pattern not found')

old2 = '"t":%.1f,"hu":%.1f,'
new2 = '"t":%.2f,"hu":%.2f,'
if old2 in content:
    content = content.replace(old2, new2)
    print('OK: t/hu changed to %.2f')
else:
    print('SKIP: t/hu pattern not found')

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Done.')
