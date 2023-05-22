import os
from criv_io.criv_writer import CrivWriter
# Hide the pygame welcome message
os.environ['PYGAME_HIDE_SUPPORT_PROMPT'] = "hide"
import pygame

from criv_io.image_permuter import ImagePermuter

# Class for viewing .criv files
class CrivViewer:
    # Views a .criv image file on a pygame window
    def view_criv(criv_image_path, perms, rgn_counts, window_resolution = [1920, 1080]):
        pygame.init()
        # Change window's title
        pygame.display.set_caption('Crivate')
        # Change window's icon
        logo = pygame.image.load('logo.png')
        pygame.display.set_icon(logo)
        # Set up the drawing window
        screen = pygame.display.set_mode(window_resolution)

        # Open .criv file for reading
        with open(criv_image_path, 'r') as file:
            # Read all data from the .criv file into a single list
            criv_data = file.read().split(CrivWriter.CRIV_SEPARATOR)
            # Extract the image resolution from criv data
            image_resolution = [int(criv_data[0]), int(criv_data[1])]
            # Traverse all pixels of the window
            for y in range(0, window_resolution[1]):
                for x in range(0, window_resolution[0]):
                    pos_window = [x, y]
                    # Convert window pixel coordinates to image pixel coordinates
                    pos_pixel = [
                        int(x * image_resolution[0] / window_resolution[0]),
                        int(y * image_resolution[1] / window_resolution[1])
                    ]
                    pos_perm_pixel = ImagePermuter.permute_pixel_position(
                        pos_pixel,
                        image_resolution,
                        perms,
                        rgn_counts
                    )
                    # Calculate index of the pixel in the image
                    pos_perm_pixel_idx = pos_perm_pixel[1] * image_resolution[0] + pos_perm_pixel[0]
                    # Get color of the pixel by extracting the RGB values from criv data using the pixel index
                    color = (
                        int(criv_data[2 + pos_perm_pixel_idx * 3 + 0]),
                        int(criv_data[2 + pos_perm_pixel_idx * 3 + 1]),
                        int(criv_data[2 + pos_perm_pixel_idx * 3 + 2])
                    )
                    # Draw a square with the color of the image at that point
                    pygame.draw.rect(
                        screen,
                        color,
                        pygame.Rect(pos_window[0], pos_window[1], 1, 1)
                    )

        # Update the display
        pygame.display.update()

        # Run until the user asks to quit
        running = True
        while running:
            # Did the user click the window close button?
            for event in pygame.event.get():
                if event.type == pygame.QUIT or pygame.key.get_pressed()[pygame.K_ESCAPE]:
                    running = False

        # Done! Time to quit.
        pygame.quit()