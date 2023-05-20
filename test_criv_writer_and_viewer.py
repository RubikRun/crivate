from criv_io.criv_writer import CrivWriter
from criv_io.criv_viewer import CrivViewer
from utils.math_utils import MathUtils
from random import randint
import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame
pygame.init()

criv_expected_size = 120612940
image_name = 'hires00'
og_image_path = 'original_images/' + image_name + '.png'
criv_image_path = 'encrypted_images/' + image_name + '.criv'

# Get permutation and regions for permuting the image in the .criv file
rgn_count = [24,3]
perm_size = rgn_count[0] * rgn_count[1]
perm = MathUtils.get_perm_from_index(
    perm_size,
    randint(0, MathUtils.factorial(perm_size) - 1)
)
inv_perm = MathUtils.get_inverse_perm(perm)

# Write the .criv file
CrivWriter.write_criv(
    og_image_path,
    criv_image_path,
    perm,
    rgn_count
)

# View the .criv file
CrivViewer.view_criv(
    criv_image_path,
    inv_perm,
    rgn_count,
    [int(1920 / 2), int(1080 / 2)]
)

pygame.quit()