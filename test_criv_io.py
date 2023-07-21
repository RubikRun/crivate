from criv_io.criv_writer import CrivWriter
from criv_io.criv_viewer import CrivViewer
from criv_io.criv_key import CrivKey
from random import randint
import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame
pygame.init()

image_name = 'lores01'
key_name = image_name
key_path = "keys/" + key_name + ".crivk"
og_image_path = 'original_images/' + image_name + '.png'
criv_image_path = 'encrypted_images/' + image_name + '.criv'

image = pygame.image.load(og_image_path)
image_resolution = [image.get_width(), image.get_height()]

screen_res = [1366, 768]
view_res_factor = 0.85 * min(screen_res[0] / image_resolution[0], screen_res[1] / image_resolution[1])
view_res = [int(image_resolution[0] * view_res_factor), int(image_resolution[1] * view_res_factor)]

criv_key = CrivKey.generate_random_key(image_resolution)
criv_key.write_to_file(key_path)

# Write the .criv file
CrivWriter.write_criv(
    og_image_path,
    criv_image_path,
    criv_key
)

criv_key = "lost"
criv_key = CrivKey.read_from_file(key_path)
inv_criv_key = CrivKey.get_inverse_key(criv_key)

# View the .criv file
CrivViewer.view_criv(
    criv_image_path,
    inv_criv_key, # use CrivKey.generate_identity_key(), # here to see what the image looks like encrypted
    view_res
)

pygame.quit()