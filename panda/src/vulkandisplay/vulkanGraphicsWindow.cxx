/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file vulkanGraphicsWindow.cxx
 * @author rdb
 * @date 2016-02-16
 */

#include "vulkanGraphicsWindow.h"
#include "vulkanGraphicsStateGuardian.h"

TypeHandle VulkanGraphicsWindow::_type_handle;

/**
 *
 */
VulkanGraphicsWindow::
VulkanGraphicsWindow(GraphicsEngine *engine, GraphicsPipe *pipe,
                  const string &name,
                  const FrameBufferProperties &fb_prop,
                  const WindowProperties &win_prop,
                  int flags,
                  GraphicsStateGuardian *gsg,
                  GraphicsOutput *host) :
  BaseGraphicsWindow(engine, pipe, name, fb_prop, win_prop, flags, gsg, host),
  _surface(VK_NULL_HANDLE),
  _swapchain(VK_NULL_HANDLE),
  _render_pass(VK_NULL_HANDLE),
  _present_complete(VK_NULL_HANDLE),
  _current_clear_mask(-1),
  _depth_stencil_tc(NULL),
  _image_index(0)
{
}

/**
 *
 */
VulkanGraphicsWindow::
~VulkanGraphicsWindow() {
}

/**
 * Clears the entire framebuffer before rendering, according to the settings
 * of get_color_clear_active() and get_depth_clear_active() (inherited from
 * DrawableRegion).
 *
 * This function is called only within the draw thread.
 */
void VulkanGraphicsWindow::
clear(Thread *current_thread) {
  // We do the clear in begin_frame(), and the validation layers don't like it
  // if an extra clear is being done at the beginning of a frame.  That's why
  // this is empty for now.  Need a cleaner solution for this.
}

/**
 * This function will be called within the draw thread before beginning
 * rendering for a given frame.  It should do whatever setup is required, and
 * return true if the frame should be rendered, or false if it should be
 * skipped.
 */
bool VulkanGraphicsWindow::
begin_frame(FrameMode mode, Thread *current_thread) {
  begin_frame_spam(mode);
  if (_gsg == (GraphicsStateGuardian *)NULL) {
    return false;
  }

  if (!get_unexposed_draw() && !_got_expose_event) {
    if (vulkandisplay_cat.is_spam()) {
      vulkandisplay_cat.spam()
        << "Not drawing " << this << ": unexposed.\n";
    }
    return false;
  }

  if (vulkandisplay_cat.is_spam()) {
    vulkandisplay_cat.spam()
      << "Drawing " << this << ": exposed.\n";
  }

  /*if (mode != FM_render) {
    return true;
  }*/

  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkgsg, _gsg, false);
  //vkgsg->reset_if_new();

  if (_current_clear_mask != _clear_mask) {
    // The clear flags have changed.  Recreate the render pass.  Note that the
    // clear flags don't factor into render pass compatibility, so we don't
    // need to recreate the framebuffer.
    vkQueueWaitIdle(vkgsg->_queue);
    setup_render_pass();
  }

  if (_swapchain_size != _size) {
    // Uh-oh, the window must have resized.  Recreate the swapchain.
    // Before destroying the old, make sure the queue is no longer rendering
    // anything to it.
    vkQueueWaitIdle(vkgsg->_queue);
    destroy_swapchain();
    if (!create_swapchain()) {
      return false;
    }
  }

  // Instruct the GSG that we are commencing a new frame.  This will cause it
  // to create a command buffer.
  vkgsg->set_current_properties(&get_fb_properties());
  if (!vkgsg->begin_frame(current_thread)) {
    return false;
  }

  if (mode != FM_render) {
    return true;
  }

  VkSemaphoreCreateInfo semaphore_info;
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semaphore_info.pNext = NULL;
  semaphore_info.flags = 0;

  VkResult err;
  err = vkCreateSemaphore(vkgsg->_device, &semaphore_info,
                          NULL, &_present_complete);
  nassertr(err == 0, false);

  if (mode == FM_render) {
    err = vkAcquireNextImageKHR(vkgsg->_device, _swapchain, UINT64_MAX,
                                _present_complete, (VkFence)0, &_image_index);

    nassertr(_image_index < _swap_buffers.size(), false);
  }
  SwapBuffer &buffer = _swap_buffers[_image_index];

  /*if (mode == FM_render) {
    clear_cube_map_selection();
  }*/

  // Now that we have a command buffer, start our render pass.  First
  // transition the swapchain images into the valid state for rendering into.
  VkCommandBuffer cmd = vkgsg->_cmd;

  VkClearValue clears[2];
  LColor clear_color = get_clear_color();
  clears[0].color.float32[0] = clear_color[0];
  clears[0].color.float32[1] = clear_color[1];
  clears[0].color.float32[2] = clear_color[2];
  clears[0].color.float32[3] = clear_color[3];

  VkRenderPassBeginInfo begin_info;
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.pNext = NULL;
  begin_info.renderPass = _render_pass;
  begin_info.framebuffer = buffer._framebuffer;
  begin_info.renderArea.offset.x = 0;
  begin_info.renderArea.offset.y = 0;
  begin_info.renderArea.extent.width = _size[0];
  begin_info.renderArea.extent.height = _size[1];
  begin_info.clearValueCount = 1;
  begin_info.pClearValues = clears;

  if (!get_clear_color_active()) {
    // If we aren't clearing (which is a bad idea - please clear the window)
    // then we need to transition it to a consistent state..
    if (buffer._tc->_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      // If the attachment is set to LOAD, we need to clear it for the first
      // time if we don't want the validation layer to yell at us.
      // We clear it to an arbitrary arbitrary color.  We'll just pick the
      // color returned by get_clear_color(), even if it is meaningless.
      buffer._tc->clear_color_image(cmd, clears[0].color);
    }

    buffer._tc->transition(cmd, vkgsg->_graphics_queue_family_index,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  } else {
    // This transition will be made when the first subpass is started.
    buffer._tc->_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    buffer._tc->_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    buffer._tc->_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }

  if (_depth_stencil_tc != NULL) {
    begin_info.clearValueCount++;
    clears[1].depthStencil.depth = get_clear_depth();
    clears[1].depthStencil.stencil = get_clear_stencil();

    // Transition the depth-stencil image to a consistent state.
    if (!get_clear_depth_active() || !get_clear_stencil_active()) {
      _depth_stencil_tc->transition(cmd, vkgsg->_graphics_queue_family_index,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    } else {
      // This transition will be made when the first subpass is started.
      _depth_stencil_tc->_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      _depth_stencil_tc->_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      _depth_stencil_tc->_stage_mask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
  }

  vkCmdBeginRenderPass(cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
  vkgsg->_render_pass = _render_pass;
  vkgsg->_fb_color_tc = buffer._tc;
  vkgsg->_fb_depth_tc = _depth_stencil_tc;

  return true;
}

/**
 * This function will be called within the draw thread after rendering is
 * completed for a given frame.  It should do whatever finalization is
 * required.
 */
void VulkanGraphicsWindow::
end_frame(FrameMode mode, Thread *current_thread) {
  end_frame_spam(mode);

  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_V(vkgsg, _gsg);

  if (mode == FM_render) {
    VkCommandBuffer cmd = vkgsg->_cmd;
    nassertv(cmd != VK_NULL_HANDLE);

    vkCmdEndRenderPass(cmd);
    vkgsg->_render_pass = VK_NULL_HANDLE;

    // The driver implicitly transitioned this to the final layout.
    _swap_buffers[_image_index]._tc->_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Now we can do copy-to-texture, now that the render pass has ended.
    copy_to_textures();
  }

  // Note: this will close the command buffer.
  vkgsg->end_frame(current_thread);

  if (mode == FM_render) {
    nassertv(_present_complete != VK_NULL_HANDLE);
    trigger_flip();
    clear_cube_map_selection();
  }
}

/**
 * This function will be called within the draw thread after end_frame() has
 * been called on all windows, to initiate the exchange of the front and back
 * buffers.
 *
 * This should instruct the window to prepare for the flip at the next video
 * sync, but it should not wait.
 *
 * We have the two separate functions, begin_flip() and end_flip(), to make it
 * easier to flip all of the windows at the same time.
 */
void VulkanGraphicsWindow::
begin_flip() {
}

/**
 * This function will be called within the draw thread after end_frame() has
 * been called on all windows, to initiate the exchange of the front and back
 * buffers.
 *
 * This should instruct the window to prepare for the flip when command, but
 * will not actually flip
 *
 * We have the two separate functions, begin_flip() and end_flip(), to make it
 * easier to flip all of the windows at the same time.
 */
void VulkanGraphicsWindow::
ready_flip() {
}

/**
 * This function will be called within the draw thread after begin_flip() has
 * been called on all windows, to finish the exchange of the front and back
 * buffers.
 *
 * This should cause the window to wait for the flip, if necessary.
 */
void VulkanGraphicsWindow::
end_flip() {
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_V(vkgsg, _gsg);
  VkDevice device = vkgsg->_device;
  VkQueue queue = vkgsg->_queue;
  VkResult err;

  SwapBuffer &buffer = _swap_buffers[_image_index];
  nassertv(buffer._tc->_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  VkResult results[1];
  VkPresentInfoKHR present;
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.pNext = NULL;
  present.waitSemaphoreCount = 0;
  present.pWaitSemaphores = NULL;
  //present.waitSemaphoreCount = 1;
  //present.pWaitSemaphores = &_present_complete;
  present.swapchainCount = 1;
  present.pSwapchains = &_swapchain;
  present.pImageIndices = &_image_index;
  present.pResults = results;

  err = vkQueuePresentKHR(queue, &present);
  if (err == VK_ERROR_OUT_OF_DATE_KHR) {
    cerr << "out of date.\n";

  } else if (err == VK_SUBOPTIMAL_KHR) {
    cerr << "suboptimal.\n";

  } else if (err != VK_SUCCESS) {
    vulkan_error(err, "Error presenting queue");
    return;
  }

  // Should we really wait for the present to be done?  Seems like a waste of
  // precious frame time.
  err = vkQueueWaitIdle(queue);
  assert(err == VK_SUCCESS);

  vkDestroySemaphore(vkgsg->_device, _present_complete, NULL);
  _present_complete = VK_NULL_HANDLE;
}

/**
 * Closes the window right now.  Called from the window thread.
 */
void VulkanGraphicsWindow::
close_window() {
  // Destroy the previous swapchain first, if we had one.
  if (!_gsg.is_null()) {
    VulkanGraphicsStateGuardian *vkgsg;
    DCAST_INTO_V(vkgsg, _gsg);

    // Wait until the queue is done with any commands that might use the swap
    // chain, then destroy it.
    vkQueueWaitIdle(vkgsg->_queue);
    destroy_swapchain();

    if (_render_pass != VK_NULL_HANDLE) {
      vkDestroyRenderPass(vkgsg->_device, _render_pass, NULL);
      _render_pass = VK_NULL_HANDLE;
    }

    _gsg.clear();
  }
  BaseGraphicsWindow::close_window();
}

/**
 * Opens the window right now.  Called from the window thread.  Returns true
 * if the window is successfully opened, or false if there was a problem.
 */
bool VulkanGraphicsWindow::
open_window() {
  VulkanGraphicsPipe *vkpipe;
  DCAST_INTO_R(vkpipe, _pipe, false);

  if (!BaseGraphicsWindow::open_window()) {
    return false;
  }

  VkResult err;

  // Create a surface using the WSI extension.
#ifdef _WIN32
  VkWin32SurfaceCreateInfoKHR surface_info;
  surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
  surface_info.pNext = NULL;
  surface_info.flags = 0;
  surface_info.hinstance = GetModuleHandle(NULL);
  surface_info.hwnd = _hWnd;

  err = vkCreateWin32SurfaceKHR(vkpipe->_instance, &surface_info, NULL, &_surface);

#elif defined(HAVE_X11)
  VkXlibSurfaceCreateInfoKHR surface_info;
  surface_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
  surface_info.pNext = NULL;
  surface_info.flags = 0;
  surface_info.dpy = _display;
  surface_info.window = _xwindow;

  err = vkCreateXlibSurfaceKHR(vkpipe->_instance, &surface_info, NULL, &_surface);
#endif

  if (err) {
    vulkan_error(err, "Failed to create surface");
    return false;
  }

  // Make sure we have a GSG, which manages a VkDevice.
  VulkanGraphicsStateGuardian *vkgsg;
  uint32_t queue_family_index = 0;
  if (_gsg == NULL) {
    // Find a queue suitable both for graphics and for presenting to our
    // surface.  TODO: fall back to separate graphics/present queues?
    if (!vkpipe->find_queue_family_for_surface(queue_family_index, _surface, VK_QUEUE_GRAPHICS_BIT)) {
      vulkan_error(err, "Failed to find graphics queue that can present to surface");
      return false;
    }

    // There is no old gsg.  Create a new one.
    vkgsg = new VulkanGraphicsStateGuardian(_engine, vkpipe, NULL, queue_family_index);
    _gsg = vkgsg;
  } else {
    //TODO: check that the GSG's queue can present to our surface.
  }

  _fb_properties.set_force_hardware(vkgsg->is_hardware());
  _fb_properties.set_force_software(!vkgsg->is_hardware());

  // Query the preferred image formats for this surface.
  uint32_t num_formats;
  err = vkGetPhysicalDeviceSurfaceFormatsKHR(vkpipe->_gpu, _surface,
                                             &num_formats, NULL);
  nassertr(!err, false);
  VkSurfaceFormatKHR *formats =
      (VkSurfaceFormatKHR *)alloca(sizeof(VkSurfaceFormatKHR) * num_formats);
  err = vkGetPhysicalDeviceSurfaceFormatsKHR(vkpipe->_gpu, _surface,
                                             &num_formats, formats);
  nassertr(!err, false);

  // If the format list includes just one entry of VK_FORMAT_UNDEFINED, the
  // surface has no preferred format.  Otherwise, at least one supported
  // format will be returned.
  //TODO: add more logic for picking a suitable format.
  if (num_formats == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
    if (_fb_properties.get_srgb_color()) {
      _surface_format.format = VK_FORMAT_B8G8R8A8_SRGB;
    } else {
      _surface_format.format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    _surface_format.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  } else {
    // Find a format that meets the requirements.
    nassertr(num_formats > 1, false);
    _surface_format = formats[0];

    if (_fb_properties.get_srgb_color()) {
      for (uint32_t i = 0U; i < num_formats; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
            formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) {
          _surface_format = formats[i];
          _fb_properties.set_rgba_bits(8, 8, 8, 8);
          break;
        }
      }
    } else {
      for (uint32_t i = 0U; i < num_formats; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
          _surface_format = formats[i];
          _fb_properties.set_rgba_bits(8, 8, 8, 8);
          break;
        }
      }
    }
  }

  // Choose a suitable depth/stencil format that satisfies the requirements.
  VkFormatProperties fmt_props;
  bool request_depth32 = _fb_properties.get_depth_bits() > 24 ||
                         _fb_properties.get_float_depth();

  if (_fb_properties.get_depth_bits() > 0 || _fb_properties.get_stencil_bits() > 0) {
    // Vulkan requires support for at least of one of these two formats.
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &fmt_props);
    bool supports_depth32 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fmt_props);
    bool supports_depth24 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    if ((supports_depth32 && request_depth32) || !supports_depth24) {
      _depth_stencil_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
      _fb_properties.set_depth_bits(32);
    } else {
      _depth_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
      _fb_properties.set_depth_bits(24);
    }
    _fb_properties.set_stencil_bits(8);

    _depth_stencil_aspect_mask |= VK_IMAGE_ASPECT_DEPTH_BIT |
                                  VK_IMAGE_ASPECT_STENCIL_BIT;

  } else if (_fb_properties.get_depth_bits() > 0) {
    // Vulkan requires support for at least of one of these two formats.
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_D32_SFLOAT, &fmt_props);
    bool supports_depth32 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    vkGetPhysicalDeviceFormatProperties(vkpipe->_gpu, VK_FORMAT_X8_D24_UNORM_PACK32, &fmt_props);
    bool supports_depth24 = (fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    if ((supports_depth32 && request_depth32) || !supports_depth24) {
      _depth_stencil_format = VK_FORMAT_D32_SFLOAT;
      _fb_properties.set_depth_bits(32);
    } else {
      _depth_stencil_format = VK_FORMAT_X8_D24_UNORM_PACK32;
      _fb_properties.set_depth_bits(24);
    }

    _depth_stencil_aspect_mask |= VK_IMAGE_ASPECT_DEPTH_BIT;

  } else {
    _depth_stencil_format = VK_FORMAT_UNDEFINED;
    _depth_stencil_aspect_mask |= 0;
  }

  return setup_render_pass() && create_swapchain();
}

/**
 * Creates a render pass object for this window.  Call this whenever the
 * format or clear parameters change.  Note that all pipeline states become
 * invalid if the render pass is no longer compatible; however, we currently
 * call this only when the clear flags change, which does not affect pipeline
 * compatibility.
 */
bool VulkanGraphicsWindow::
setup_render_pass() {
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkgsg, _gsg, false);

  if (vulkandisplay_cat.is_debug()) {
    vulkandisplay_cat.debug()
      << "Creating render pass for VulkanGraphicsWindow " << this << "\n";
  }

  // Now we want to create a render pass, and for that we need to describe the
  // framebuffer attachments as well as any subpasses we'd like to use.
  VkAttachmentDescription attachments[2];
  attachments[0].flags = 0;
  attachments[0].format = _surface_format.format;
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  attachments[1].flags = 0;
  attachments[1].format = _depth_stencil_format;
  attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  if (get_clear_color_active()) {
    // We don't care about the current contents.
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  } else {
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  }

  if (get_clear_depth_active()) {
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  }

  if (get_clear_stencil_active()) {
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  }

  if (get_clear_depth_active() && get_clear_stencil_active()) {
    // We don't care about the current contents.
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  VkAttachmentReference color_reference;
  color_reference.attachment = 0;
  color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_reference;
  depth_reference.attachment = 1;
  depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = NULL;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_reference;
  subpass.pResolveAttachments = NULL;
  subpass.pDepthStencilAttachment = _depth_stencil_format ? &depth_reference : NULL;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = NULL;

  VkRenderPassCreateInfo pass_info;
  pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  pass_info.pNext = NULL;
  pass_info.flags = 0;
  pass_info.attachmentCount = _depth_stencil_format ? 2 : 1;
  pass_info.pAttachments = attachments;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  pass_info.dependencyCount = 0;
  pass_info.pDependencies = NULL;

  VkRenderPass pass;
  VkResult
  err = vkCreateRenderPass(vkgsg->_device, &pass_info, NULL, &pass);
  if (err) {
    vulkan_error(err, "Failed to create render pass");
    return false;
  }

  // Destroy the previous render pass object.
  if (_render_pass != VK_NULL_HANDLE) {
    // Actually, we can't destroy it, since we may now have pipeline states
    // that reference it.  Destroying it now would also require destroying the
    // framebuffer and clearing all of the prepared states from the GSG.
    // Maybe we need to start reference counting render passes?
    vulkandisplay_cat.warning() << "Leaking VkRenderPass.\n";
    //vkDestroyRenderPass(vkgsg->_device, _render_pass, NULL);
    //_render_pass = VK_NULL_HANDLE;
  }

  _render_pass = pass;
  _current_clear_mask = _clear_mask;
  return true;
}

/**
 * Destroys an existing swapchain.  Before calling this, make sure that no
 * commands are executing on any queue that uses this swapchain.
 */
void VulkanGraphicsWindow::
destroy_swapchain() {
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_V(vkgsg, _gsg);
  VkDevice device = vkgsg->_device;

  // Make sure that the GSG's command buffer releases its resources.
  if (vkgsg->_cmd != VK_NULL_HANDLE) {
    vkResetCommandBuffer(vkgsg->_cmd, 0);
  }

  // Destroy the resources held for each link in the swap chain.
  SwapBuffers::iterator it;
  for (it = _swap_buffers.begin(); it != _swap_buffers.end(); ++it) {
    SwapBuffer &buffer = *it;

    // Destroy the framebuffers that use the swapchain images.
    vkDestroyFramebuffer(device, buffer._framebuffer, NULL);
    vkDestroyImageView(device, buffer._tc->_image_view, NULL);

    buffer._tc->update_data_size_bytes(0);
    delete buffer._tc;
  }
  _swap_buffers.clear();

  if (_depth_stencil_tc != NULL) {
    if (_depth_stencil_tc->_image_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, _depth_stencil_tc->_image_view, NULL);
      _depth_stencil_tc->_image_view = VK_NULL_HANDLE;
    }

    if (_depth_stencil_tc->_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, _depth_stencil_tc->_image, NULL);
      _depth_stencil_tc->_image = VK_NULL_HANDLE;
    }

    if (_depth_stencil_tc->_memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, _depth_stencil_tc->_memory, NULL);
      _depth_stencil_tc->_memory = VK_NULL_HANDLE;
    }

    delete _depth_stencil_tc;
    _depth_stencil_tc = NULL;
  }

  // Destroy the previous swapchain.  This also destroys the swapchain images.
  if (_swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device, _swapchain, NULL);
    _swapchain = VK_NULL_HANDLE;
  }

  _image_index = 0;
}

/**
 * Creates or recreates the swapchain and framebuffer.
 */
bool VulkanGraphicsWindow::
create_swapchain() {
  VulkanGraphicsPipe *vkpipe;
  VulkanGraphicsStateGuardian *vkgsg;
  DCAST_INTO_R(vkpipe, _pipe, false);
  DCAST_INTO_R(vkgsg, _gsg, false);
  VkDevice device = vkgsg->_device;
  VkResult err;

  if (vulkandisplay_cat.is_debug()) {
    vulkandisplay_cat.debug()
      << "Creating swap chain and framebuffers for VulkanGraphicsWindow " << this << "\n";
  }

  // Get the surface capabilities to make sure we make a compatible swapchain.
  VkSurfaceCapabilitiesKHR surf_caps;
  err = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkpipe->_gpu, _surface, &surf_caps);
  if (err) {
    vulkan_error(err, "Failed to get surface capabilities");
    return false;
  }

  uint32_t num_images = (uint32_t)(1 + _fb_properties.get_back_buffers());
  num_images = min(surf_caps.maxImageCount, num_images);
  num_images = max(surf_caps.minImageCount, num_images);

  // Get the supported presentation modes for this surface.
  uint32_t num_present_modes = 0;
  err = vkGetPhysicalDeviceSurfacePresentModesKHR(vkpipe->_gpu, _surface, &num_present_modes, NULL);
  VkPresentModeKHR *present_modes = (VkPresentModeKHR *)
    alloca(sizeof(VkPresentModeKHR) * num_present_modes);
  err = vkGetPhysicalDeviceSurfacePresentModesKHR(vkpipe->_gpu, _surface, &num_present_modes, present_modes);

  // Note that we set the usage to include VK_IMAGE_USAGE_TRANSFER_SRC_BIT
  // since we can at any time be asked to copy the framebuffer to a texture.
  VkSwapchainCreateInfoKHR swapchain_info;
  swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchain_info.pNext = NULL;
  swapchain_info.surface = _surface;
  swapchain_info.minImageCount = num_images;
  swapchain_info.imageFormat = _surface_format.format;
  swapchain_info.imageColorSpace = _surface_format.colorSpace;
  swapchain_info.imageExtent.width = _size[0];
  swapchain_info.imageExtent.height = _size[1];
  swapchain_info.imageArrayLayers = 1;
  swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchain_info.queueFamilyIndexCount = 0;
  swapchain_info.pQueueFamilyIndices = NULL;
  swapchain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapchain_info.clipped = true;
  swapchain_info.oldSwapchain = NULL;

  // Choose a present mode.  Use FIFO mode as fallback, which is always
  // available.  TODO: respect sync_video when choosing a mode.
  swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  for (size_t i = 0; i < num_present_modes; ++i) {
    if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
      // This is the lowest-latency non-tearing mode, so we'll take this.
      swapchain_info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
      break;
    }
    if (present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
      // This is the fastest present mode, though it tears, so we'll use this
      // if mailbox mode isn't available.
      swapchain_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }

  err = vkCreateSwapchainKHR(device, &swapchain_info, NULL, &_swapchain);
  if (err) {
    vulkan_error(err, "Failed to create swap chain");
    return false;
  }

  // Get the images in the swap chain, which may be more than requested.
  vkGetSwapchainImagesKHR(device, _swapchain, &num_images, NULL);
  _swap_buffers.resize(num_images);
  _fb_properties.set_back_buffers(num_images - 1);
  _image_index = 0;

  memset(&_swap_buffers[0], 0, sizeof(SwapBuffer) * num_images);

  VkImage *images = (VkImage *)alloca(sizeof(VkImage) * num_images);
  vkGetSwapchainImagesKHR(device, _swapchain, &num_images, images);

  PreparedGraphicsObjects *pgo = vkgsg->get_prepared_objects();
  // Now create an image view for each image.
  for (uint32_t i = 0; i < num_images; ++i) {
    SwapBuffer &buffer = _swap_buffers[i];
    buffer._tc = new VulkanTextureContext(pgo, images[i], swapchain_info.imageFormat);
    buffer._tc->_aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;
    buffer._tc->_extent.width = swapchain_info.imageExtent.width;
    buffer._tc->_extent.height = swapchain_info.imageExtent.height;
    buffer._tc->_extent.depth = 1;
    buffer._tc->_mip_levels = 1;
    buffer._tc->_array_layers = 1;

    VkImageViewCreateInfo view_info;
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = NULL;
    view_info.flags = 0;
    view_info.image = images[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = swapchain_info.imageFormat;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    err = vkCreateImageView(device, &view_info, NULL, &buffer._tc->_image_view);
    if (err) {
      vulkan_error(err, "Failed to create image view for swapchain");
      return false;
    }
  }

  // Now create a depth image.
  _depth_stencil_tc = NULL;
  bool have_ds = (_depth_stencil_format != VK_FORMAT_UNDEFINED);

  if (have_ds) {
    VkImageCreateInfo depth_img_info;
    depth_img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_img_info.pNext = NULL;
    depth_img_info.flags = 0;
    depth_img_info.imageType = VK_IMAGE_TYPE_2D;
    depth_img_info.format = _depth_stencil_format;
    depth_img_info.extent.width = _size[0];
    depth_img_info.extent.height = _size[1];
    depth_img_info.extent.depth = 1;
    depth_img_info.mipLevels = 1;
    depth_img_info.arrayLayers = 1;
    depth_img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_img_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    depth_img_info.queueFamilyIndexCount = 0;
    depth_img_info.pQueueFamilyIndices = NULL;
    depth_img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage depth_stencil_image;
    err = vkCreateImage(device, &depth_img_info, NULL, &depth_stencil_image);
    if (err) {
      vulkan_error(err, "Failed to create depth image");
      return false;
    }

    // Get the memory requirements, and find an appropriate heap to alloc in.
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, depth_stencil_image, &mem_reqs);

    VkMemoryAllocateInfo alloc_info;
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.allocationSize = mem_reqs.size;

    if (!vkpipe->find_memory_type(alloc_info.memoryTypeIndex, mem_reqs.memoryTypeBits, 0)) {
      vulkan_error(err, "Failed to find memory heap to allocate depth buffer");
      return false;
    }

    VkDeviceMemory depth_stencil_memory;
    err = vkAllocateMemory(device, &alloc_info, NULL, &depth_stencil_memory);
    if (err) {
      vulkan_error(err, "Failed to allocate memory for depth image");
      return false;
    }

    // Bind memory to image.
    err = vkBindImageMemory(device, depth_stencil_image, depth_stencil_memory, 0);
    if (err) {
      vulkan_error(err, "Failed to bind memory to depth image");
      return false;
    }

    VkImageViewCreateInfo view_info;
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = NULL;
    view_info.flags = 0;
    view_info.image = depth_stencil_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_img_info.format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = _depth_stencil_aspect_mask;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (_fb_properties.get_stencil_bits()) {
      view_info.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    VkImageView depth_stencil_view;
    err = vkCreateImageView(device, &view_info, NULL, &depth_stencil_view);
    if (err) {
      vulkan_error(err, "Failed to create image view for depth/stencil");
      return false;
    }

    _depth_stencil_tc = new VulkanTextureContext(pgo, depth_stencil_image, view_info.format);
    _depth_stencil_tc->_extent = depth_img_info.extent;
    _depth_stencil_tc->_mip_levels = depth_img_info.mipLevels;
    _depth_stencil_tc->_array_layers = depth_img_info.arrayLayers;
    _depth_stencil_tc->_aspect_mask = _depth_stencil_aspect_mask;
    _depth_stencil_tc->_memory = depth_stencil_memory;
    _depth_stencil_tc->_image_view = depth_stencil_view;
  }

  // Now finally create a framebuffer for each link in the swap chain.
  VkImageView attach_views[2];
  if (have_ds) {
    attach_views[1] = _depth_stencil_tc->_image_view;
  }

  VkFramebufferCreateInfo fb_info;
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.pNext = NULL;
  fb_info.flags = 0;
  fb_info.renderPass = _render_pass;
  fb_info.attachmentCount = 1 + (int)have_ds;
  fb_info.pAttachments = attach_views;
  fb_info.width = _size[0];
  fb_info.height = _size[1];
  fb_info.layers = 1;

  for (uint32_t i = 0; i < num_images; ++i) {
    SwapBuffer &buffer = _swap_buffers[i];
    attach_views[0] = buffer._tc->_image_view;
    err = vkCreateFramebuffer(device, &fb_info, NULL, &buffer._framebuffer);
    if (err) {
      vulkan_error(err, "Failed to create framebuffer");
      return false;
    }
  }

  _swapchain_size = _size;
  return true;
}