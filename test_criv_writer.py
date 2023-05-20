from criv_io.criv_writer import CrivWriter
import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame
pygame.init()

criv_expected_size = 120612940
image_name = 'midres01'
og_image_path = 'original_images/' + image_name + '.png'
criv_image_path = 'encrypted_images/' + image_name + '.criv'

# Write the .criv file
CrivWriter.write_criv(
    og_image_path,
    criv_image_path
)

# Check if the resulting .criv file has correct size
if os.stat(criv_image_path).st_size == criv_expected_size:
    print("TEST SUCCESS: criv_writer")
else:
    print("TEST FAIL: criv_writer")

pygame.quit()