from criv_io.criv_writer import CrivWriter
from criv_io.criv_viewer import CrivViewer
from utils.math_utils import MathUtils
from random import randint
import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame
pygame.init()

image_name = 'lores01'
og_image_path = 'original_images/' + image_name + '.png'
criv_image_path = 'encrypted_images/' + image_name + '.criv'

# Create permutation and regions for permuting the image in the .criv file
rgn_counts = [
    [24,24],
    [12,12],
    [3,3]
]
perm_sizes = []
perms = []
inv_perms = []
for i in range(len(rgn_counts)):
    perm_sizes.append(rgn_counts[i][0] * rgn_counts[i][1])
    perms.append(
        MathUtils.get_perm_from_index(
            perm_sizes[i],
            randint(1, MathUtils.factorial(perm_sizes[i]) - 1)
        )
    )
    print("Permutation at level " + str(i) + " is ", end='')
    print(perms[i])
    inv_perms.append(MathUtils.get_inverse_perm(perms[i]))

# Write the .criv file
CrivWriter.write_criv(
    og_image_path,
    criv_image_path,
    perms,
    rgn_counts
)

# View the .criv file
CrivViewer.view_criv(
    criv_image_path,
    inv_perms,
    rgn_counts,
    # screen is 1366 by 768 
    # image is 1008 by 1428
    [int(1366 / 1.5), int(768 / 1.5)]
)

pygame.quit()