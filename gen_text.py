import random
import string

# Generate a large random text of approximately 100MB
large_text_size = 100 * 1024 * 1024  # 100MB
chunk_size = 10 * 1024  # 10KB chunks for efficiency

with open("large_random_text.txt", "w") as file:
    while large_text_size > 0:
        chunk = ''.join(random.choices(string.ascii_letters + string.digits + " \n", k=min(chunk_size, large_text_size)))
        file.write(chunk)
        large_text_size -= len(chunk)

"large_random_text.txt generated with approximately 100MB of random content."
