import os
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame

# Class for writing .criv image files
class CrivWriter:
    CRIV_SEPARATOR = ','

    # Writes a .criv image file reading it from a standard image file
    def write_criv(og_image_path, criv_image_path):
        # Load the original image
        image = pygame.image.load(og_image_path)
        # Get image resolution
        image_resolution = [image.get_width(), image.get_height()]
        # Open the .criv file for writing
        with open(criv_image_path, 'w') as file:
            # First write the resolution
            CrivWriter.__write_num(file, image_resolution[0])
            CrivWriter.__write_num(file, image_resolution[1])
            # Traverse pixels one by one
            for y in range(image_resolution[1]):
                for x in range(image_resolution[0]):
                    # Get current pixel color
                    color = image.get_at([x,y])
                    # Write the pixel color to the .criv file,
                    # as 3 RGB components, 3 ints between 0 and 255
                    CrivWriter.__write_num(file, color[0])
                    CrivWriter.__write_num(file, color[1])
                    CrivWriter.__write_num(file, color[2])

    # Writes a single number to an opened .criv file
    def __write_num(file, num):
        file.write(str(num) + CrivWriter.CRIV_SEPARATOR)