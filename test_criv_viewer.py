from criv_io.criv_viewer import CrivViewer
import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame
pygame.init()

image_name = 'hires00'
criv_image_path = 'encrypted_images/' + image_name + '.criv'

# View the .criv file
CrivViewer.view_criv(criv_image_path, [int(1920 / 2), int(1080 / 2)])

pygame.quit()