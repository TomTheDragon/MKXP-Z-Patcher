# Added by the MKXP-Z Patcher.
# Fullscreen++ (Zeus81, VX / VX Ace) resizes the game window through Win32 calls
# on the "RGSS Player" window, which doesn't exist under mkxp-z, and keeps its
# settings with GetPrivateProfileString (Windows only). This preload replaces
# those parts, as soon as the game's script defines them, with mkxp-z's own
# window functions, so its keys keep working on every platform:
#   F5  fullscreen on/off      F6  next window size (1x, 2x, ... fit screen)
# In a widescreen mode (F1 menu) the window sizes are 16:9.
# Settings stay in Game.ini, section [Fullscreen++], as with the original.
# mkxp-z's own Alt+Enter is followed too.

module MkxpzFullscreenPlus
  SECTION = 'Fullscreen++'

  class << Graphics
    alias_method :mkxpz_fspp_resize_window, :resize_window
  end

  def self.active?
    $imported.is_a?(Hash) && $imported[:Zeus_Fullscreen] ? true : false
  end

  # --- Game.ini, section [Fullscreen++] (plain Ruby, works everywhere)
  def self.ini_path
    Dir.entries('.').find { |f| f.casecmp?('Game.ini') } || 'Game.ini'
  rescue
    'Game.ini'
  end

  def self.ini_read(key, default)
    text = (File.binread(ini_path) rescue nil)
    return default unless text
    in_section = false
    text.each_line do |line|
      s = line.strip
      if s.start_with?('[')
        in_section = s.casecmp?("[#{SECTION}]")
      elsif in_section
        k, v = s.split('=', 2)
        return v.strip if v && k.strip.casecmp?(key)
      end
    end
    default
  end

  def self.ini_write(values)
    path = ini_path
    text = (File.binread(path) rescue '').b
    nl = text.include?("\r\n") ? "\r\n" : "\n"
    lines = text.split(/\r?\n/, -1)
    lines.pop if lines.last == ''
    start = lines.index { |l| l.strip.casecmp?("[#{SECTION}]") }
    unless start
      lines << '' unless lines.empty? || lines.last.strip.empty?
      lines << "[#{SECTION}]"
      start = lines.size - 1
    end
    stop = start + 1
    stop += 1 while stop < lines.size && !lines[stop].strip.start_with?('[')
    values.each do |key, value|
      entry = "#{key}=#{value}".b
      i = (start + 1...stop).find { |j| (k = lines[j].split('=', 2)[0]) && k.strip.casecmp?(key) }
      if i
        lines[i] = entry
      else
        last = (start...stop).to_a.reverse.find { |j| !lines[j].strip.empty? }
        lines.insert(last + 1, entry)
        stop += 1
      end
    end
    File.binwrite(path, lines.join(nl) + nl)
  rescue => e
    System.puts("[Fullscreen++] couldn't save settings: #{e.message}") rescue nil
  end

  # --- window
  # The window's width at the screen's height: wider than the screen in the
  # widescreen modes (widescreen.rb), where the window is 16:9.
  def self.frame_width
    w = defined?(MkxpzWidescreen) ? MkxpzWidescreen.frame_w : nil
    w && w > 0 ? w : Graphics.width
  end

  # Android: always the whole screen (see android_screen.rb)
  def self.android?
    ENV['ANDROID_ROOT'] && ENV['ANDROID_DATA'] ? true : false
  end

  def self.apply(fullscreen, w, h)
    if fullscreen || android?
      Graphics.fullscreen = true unless Graphics.fullscreen
    else
      Graphics.fullscreen = false if Graphics.fullscreen
      Graphics.mkxpz_fspp_resize_window(w, h, true)
    end
  end

  # Alt+Enter (mkxp-z's own toggle) changes the mode behind the script's back.
  def self.sync
    now = Graphics.fullscreen
    if @last.nil? || now == @last
      @last = now
      return
    end
    @last = now
    wanted = Graphics.instance_variable_get(:@fullscreen) ? true : false
    return if now == wanted
    Graphics.instance_variable_set(:@fullscreen, now)
    Graphics.ratio += 0 # resize for the new mode, save settings
  end

  # Replacements, installed into Graphics' singleton class.
  module Impl
    def initialize_fullscreen_rects
      @borders_size ||= borders_size
      @fullscreen_rect ||= screen_rect
      @workarea_rect ||= workarea_rect
    end
    def borders_size() Rect.new(0, 0, 0, 0) end
    def screen_rect() Rect.new(0, 0, display_width, display_height) end
    def workarea_rect
      # leave room for the title bar and task bars / panels
      Rect.new(0, 0, [display_width - 16, width].max, [display_height - 96, height].max)
    end
    def hide_borders() end
    def show_borders() end
    def hide_back() end
    def show_back() end
    def release_alt() end
    def resize_window(w, h)
      MkxpzFullscreenPlus.apply(@fullscreen, w, h)
    end
    # the original, with the window's width instead of the screen's
    def ratio=(r)
      return if vx_fullscreen?
      initialize_fullscreen_rects
      r = 0 if r < 0
      fw = MkxpzFullscreenPlus.frame_width
      if @fullscreen
        @fullscreen_ratio = r
        w_max, h_max = @fullscreen_rect.width, @fullscreen_rect.height
      else
        @windowed_ratio = r
        w_max = @workarea_rect.width - @borders_size.width
        h_max = @workarea_rect.height - @borders_size.height
      end
      if r == 0
        w, h = w_max, w_max * height / fw
        h, w = h_max, h_max * fw / height if h > h_max
      else
        w, h = fw * r, height * r
        return self.ratio = 0 if w > w_max or h > h_max
      end
      resize_window(w, h)
      save_fullscreen_settings
    end
    def load_fullscreen_settings
      @fullscreen       = MkxpzFullscreenPlus.ini_read('Fullscreen', '0') == '1'
      @fullscreen_ratio = MkxpzFullscreenPlus.ini_read('FullscreenRatio', '0').to_i
      @windowed_ratio   = MkxpzFullscreenPlus.ini_read('WindowedRatio', '1').to_i
      fullscreen? ? fullscreen_mode : windowed_mode
    end
    def save_fullscreen_settings
      MkxpzFullscreenPlus.ini_write([['Fullscreen', @fullscreen ? '1' : '0'],
                                     ['FullscreenRatio', @fullscreen_ratio.to_i],
                                     ['WindowedRatio', @windowed_ratio.to_i]])
    end
    # the original's "VX fullscreen" (640x480 display mode) has no equivalent;
    # it becomes the normal fullscreen
    def vx_fullscreen?() false end
    def toggle_vx_fullscreen() toggle_fullscreen end
    def vx_fullscreen_mode() fullscreen_mode end
    def update
      zeus_fullscreen_update
      MkxpzFullscreenPlus.sync
      toggle_fullscreen if Input.trigger?(Input::F5)
      toggle_ratio      if Input.trigger?(Input::F6)
    end
  end

  NAMES = Impl.instance_methods(false)

  # Only definitions made by the Fullscreen++ script itself are replaced (it
  # stores its own __FILE__ in $imported); later scripts that redefine e.g.
  # Graphics.update on top of it keep theirs.
  def self.install(name, file)
    return if @installing || !NAMES.include?(name) || !active? || file != $imported[:Zeus_Fullscreen]
    @installing = true
    begin
      Graphics.singleton_class.send(:define_method, name, Impl.instance_method(name))
    ensure
      @installing = false
    end
  end
end

# Swap in a replacement whenever the game's script defines one of these methods
# (it then calls Graphics.load_fullscreen_settings, which already uses ours).
def Graphics.singleton_method_added(name)
  super
  where = caller_locations(1, 1)[0]
  MkxpzFullscreenPlus.install(name, where && where.path)
end
