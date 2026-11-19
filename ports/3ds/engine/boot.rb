# MKXP-Z for the 3DS: the Ruby side of the RGSS classes, and the start of
# the game (its scripts, like RPG Maker's player runs them).
# $MKXPZ_RGSS (1 XP, 2 VX, 3 VX Ace) and $MKXPZ_SCRIPTS are set by main.c.

class RGSSError < StandardError; end
class RGSSReset < Exception; end

class Font
  class << self
    attr_accessor :default_name, :default_size, :default_bold, :default_italic,
                  :default_outline, :default_shadow, :default_color, :default_out_color
  end
  attr_accessor :name, :size, :bold, :italic, :outline, :shadow, :color, :out_color

  def initialize(name = Font.default_name, size = Font.default_size)
    @name = name.is_a?(String) ? name.dup : name
    @size = size
    @bold = Font.default_bold
    @italic = Font.default_italic
    @outline = Font.default_outline
    @shadow = Font.default_shadow
    @color = Font.default_color.dup
    @out_color = Font.default_out_color.dup
  end

  def initialize_copy(other)
    super
    @color = other.color.dup
    @out_color = other.out_color.dup
  end

  def color=(c)
    @color = c.dup
  end

  def out_color=(c)
    @out_color = c.dup
  end
end

case $MKXPZ_RGSS
when 3
  Font.default_name = ['VL Gothic']
  Font.default_size = 24
  Font.default_outline = true
  Font.default_shadow = false
when 2
  Font.default_name = ['UmePlus Gothic', 'MS PGothic', 'VL PGothic', 'VL Gothic']
  Font.default_size = 20
  Font.default_outline = false
  Font.default_shadow = true
else
  Font.default_name = ['MS PGothic', 'VL PGothic', 'VL Gothic']
  Font.default_size = 22
  Font.default_outline = false
  Font.default_shadow = false
end
Font.default_bold = false
Font.default_italic = false
Font.default_color = Color.new(255, 255, 255, 255)
Font.default_out_color = Color.new(0, 0, 0, 128)

# no sound yet
module Audio
  module_function

  def bgm_play(*) end
  def bgm_stop(*) end
  def bgm_fade(*) end
  def bgm_pos; 0; end
  def bgs_play(*) end
  def bgs_stop(*) end
  def bgs_fade(*) end
  def bgs_pos; 0; end
  def me_play(*) end
  def me_stop(*) end
  def me_fade(*) end
  def se_play(*) end
  def se_stop(*) end
  def setup_midi(*) end
end

# Windows functions: none on the 3DS; calls do nothing
class Win32API
  def initialize(*) end
  def call(*) 0 end
  alias Call call
end

# XP: not drawn yet; maps are empty (VX / VX Ace: tilemap.c)
unless defined?(Tilemap)
class Tilemap
  attr_accessor :bitmaps, :map_data, :flash_data, :flags, :viewport, :visible, :ox, :oy,
                :tileset, :autotiles, :priorities
  def initialize(viewport = nil)
    @viewport = viewport
    @bitmaps = []
    @autotiles = []
    @visible = true
    @ox = @oy = 0
    @disposed = false
  end
  def dispose; @disposed = true; end
  def disposed?; @disposed; end
  def update; end
end
end

module Kernel
  def load_data(filename)
    File.open(filename, 'rb') { |f| Marshal.load(f) }
  end

  def save_data(obj, filename)
    File.open(filename, 'wb') { |f| Marshal.dump(obj, f) }
  end

  def msgbox(*args)
    $stdout.puts(args.map(&:to_s).join)
    nil
  end

  def msgbox_p(*args)
    $stdout.puts(args.map(&:inspect).join("\n"))
    nil
  end

  def rgss_main
    begin
      yield
    rescue RGSSReset
      GC.start
      retry
    end
  end

  def rgss_stop
    loop { Graphics.update }
  end
end
# XP / VX print with message boxes too
alias print msgbox if $MKXPZ_RGSS >= 2

$stdout.sync = true

def mkxpz_run_scripts
  $RGSS_SCRIPTS = load_data($MKXPZ_SCRIPTS)
  $RGSS_SCRIPTS.each_with_index do |s, i|
    s[3] = Zlib::Inflate.inflate(s[2]).force_encoding(Encoding::UTF_8)
  end
  test = File.exist?('mkxpz-test.rb')
  $RGSS_SCRIPTS.each_with_index do |s, i|
    name = s[1].to_s.force_encoding(Encoding::UTF_8)
    # for testing: mkxpz-test.rb in the game folder, before Main (which doesn't return)
    if test && name.strip == 'Main'
      eval(File.read('mkxpz-test.rb'), TOPLEVEL_BINDING, 'mkxpz-test.rb')
      test = false
    end
    eval(s[3], TOPLEVEL_BINDING, name.empty? ? "Section#{i}" : name)
  end
end
