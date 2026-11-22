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

# Windows functions: none on the 3DS; calls do nothing
class Win32API
  def initialize(*) end
  def call(*) 0 end
  alias Call call
end

# Screenshots in save files ("快速存储Bitmap的Marshal" by 柳之一, e.g. in
# Changed): the script's Bitmap#_dump / Bitmap._load copy the original
# engine's pixel memory with RtlMoveMemory, which doesn't exist here (the
# screenshot came out empty). Replaced, when a script defines them with its
# RtlMoveMemory constants, by versions with the real pixels in the original
# format (BGRA, bottom row first, zlib): saves stay compatible with the PC
# (Game.exe and the patcher's mkxp-z: its compat/bitmap_marshal.rb).
module MkxpzBitmapMarshal
  def self.dump(bmp)
    [bmp.width, bmp.height, Zlib::Deflate.deflate(bmp.mkxpz_bgra_bottom_up)].pack('LLa*')
  end

  def self.load(klass, str)
    w, h, zdata = str.unpack('LLa*')
    b = klass.new([w, 1].max, [h, 1].max)
    begin
      b.mkxpz_load_bgra_bottom_up(Zlib::Inflate.inflate(zdata))
    rescue StandardError => e
      mkxpz_log("save file image unreadable: #{e.message}")
    end
    b
  end

  def self.memory_script?
    Bitmap.constants.any? { |c| c.to_s =~ /RtlMoveMemory/i }
  rescue StandardError
    false
  end

  module Impl
    def _dump(limit)
      MkxpzBitmapMarshal.dump(self)
    end
  end

  module ClassImpl
    def _load(str)
      MkxpzBitmapMarshal.load(self, str)
    end
  end

  def self.install(target, name, impl)
    return if @installing || !memory_script?
    @installing = true
    begin
      target.send(:define_method, name, impl.instance_method(name))
      mkxpz_log("screenshots in saves: Bitmap.#{name} with real pixels")
    ensure
      @installing = false
    end
  end
end

class Bitmap
  def self.method_added(name)
    super
    MkxpzBitmapMarshal.install(self, :_dump, MkxpzBitmapMarshal::Impl) if name == :_dump && self == Bitmap
  end

  def self.singleton_method_added(name)
    super
    MkxpzBitmapMarshal.install(singleton_class, :_load, MkxpzBitmapMarshal::ClassImpl) if name == :_load && self == Bitmap
  end
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
    Graphics.mkxpz_loading
    File.open(filename, 'rb') { |f| Marshal.load(f) }
  ensure
    Graphics.mkxpz_loading
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
      mkxpz_reset_cleanup
      retry
    end
  end

  # as mkxp-z does on a reset: what's on the screen goes, the sound stops
  def mkxpz_reset_cleanup
    mkxpz_dispose_all
    Graphics.mkxpz_reset
    Audio.bgm_stop; Audio.bgs_stop; Audio.me_stop; Audio.se_stop
    GC.start
  end

  # the scripts; a reset outside rgss_main (XP and VX games have none: F12
  # ran all their scripts again) runs them again, from inside the rescue
  # like RGSS1/2 did: $! and $@ are set then, which scripts check so they
  # don't alias their methods twice ("alias ... unless $@")
  def mkxpz_run_all
    mkxpz_run_scripts
  rescue RGSSReset
    mkxpz_log("reset: running the scripts again")
    mkxpz_reset_cleanup
    mkxpz_run_all
  end

  def rgss_stop
    loop { Graphics.update }
  end
end
# XP / VX print with message boxes too
alias print msgbox if $MKXPZ_RGSS >= 2

$stdout.sync = true

# >>> mkxpz-fast-helpers (tools/fast-checksums.rb uses these too)
# a method's text, without comments and indentation
def mkxpz_norm(text)
  text.lines.map { |l| l.sub(/(\A|\s)#.*/, '').strip }.reject(&:empty?).join("\n")
end

# the text of the method defined at line of a script section: up to the
# first "end" with the indentation of its "def"
def mkxpz_method_text(script, line)
  l = script.lines
  d = l[line - 1] or return nil
  ind = d[/\A */]
  j = (line...l.size).find { |k| l[k] =~ /\A#{ind}end\b/ } or return nil
  l[(line - 1)..j].join
end
# <<< mkxpz-fast-helpers

# Faster standard VX Ace scripts: Sprite_Character#update runs in C
# (scene.c), but only while every method it stands for is the standard one.
# Checksums of the standard methods' text (ports/3ds/tools/fast-checksums.rb);
# :attr an attr_reader/accessor, :c the engine's own.
MKXPZ_STOCK = {
  'Sprite_Character#update' => 2558777199,
  'Sprite_Character#update_bitmap' => 4103765253,
  'Sprite_Character#graphic_changed?' => 3797258696,
  'Sprite_Character#update_src_rect' => 1942911822,
  'Sprite_Character#update_position' => 1236191671,
  'Sprite_Character#update_other' => 1919106263,
  'Sprite_Character#update_balloon' => 3185305667,
  'Sprite_Character#setup_new_effect' => 3675786357,
  'Sprite_Base#update' => 1901904939,
  'Sprite_Base#animation?' => 4013398038,
  'Game_CharacterBase#screen_x' => 1265888610,
  'Game_CharacterBase#screen_y' => 414283810,
  'Game_CharacterBase#screen_z' => 16537052,
  'Game_CharacterBase#shift_y' => 3177371100,
  'Game_CharacterBase#jump_height' => 1514128538,
  'Game_CharacterBase#object_character?' => 2074604390,
  'Game_CharacterBase#update' => 3737941549,
  'Game_CharacterBase#update_stop' => 3926629533,
  'Game_CharacterBase#update_animation' => 2588247382,
  'Game_CharacterBase#update_anime_count' => 2986621416,
  'Game_CharacterBase#moving?' => 3458338668,
  'Game_CharacterBase#jumping?' => 4224591806,
  'Game_CharacterBase#real_move_speed' => 680567531,
  'Game_CharacterBase#dash?' => 1832952089,
  'Game_Map#adjust_x' => 1413106839,
  'Game_Map#adjust_y' => 3928380013,
  'Game_Map#loop_horizontal?' => 812218791,
  'Game_Map#loop_vertical?' => 975953756,
  'Game_Map#screen_tile_x' => 1211119882,
  'Game_Map#screen_tile_y' => 3058309681,
  'Game_Map#width' => 4255624125,
  'Game_Map#height' => 4262117737,
  'Game_Event#refresh' => 2670169069,
  'Game_Event#find_proper_page' => 1545490489,
  'Game_Event#conditions_met?' => 3819658317,
  'Game_Event#update' => 2489914872,
  'Game_Event#update_stop' => 3832805855,
  'Game_Event#update_self_movement' => 2002458890,
  'Game_Event#near_the_screen?' => 2803408571,
  'Game_Event#stop_count_threshold' => 1758990067,
  'Game_Event#check_event_trigger_auto' => 1741691701,
  'Game_Character#update_stop' => 634477818,
  'Game_Switches#[]' => 1897994590,
  'Game_Variables#[]' => 3739613371,
  'Game_SelfSwitches#[]' => 3480227492,
  'Game_CharacterBase#tile_id' => :attr,
  'Game_CharacterBase#character_name' => :attr,
  'Game_CharacterBase#character_index' => :attr,
  'Game_CharacterBase#pattern' => :attr,
  'Game_CharacterBase#direction' => :attr,
  'Game_CharacterBase#opacity' => :attr,
  'Game_CharacterBase#blend_type' => :attr,
  'Game_CharacterBase#bush_depth' => :attr,
  'Game_CharacterBase#transparent' => :attr,
  'Game_CharacterBase#animation_id' => :attr,
  'Game_CharacterBase#balloon_id' => :attr,
  'Sprite#update' => :c,
  'Sprite#x=' => :c,
  'Sprite#y=' => :c,
  'Sprite#z=' => :c,
  'Sprite#opacity=' => :c,
  'Sprite#blend_type=' => :c,
  'Sprite#bush_depth=' => :c,
  'Sprite#visible=' => :c,
  'Sprite#src_rect' => :c,
}
MKXPZ_FAST_NEEDS = {
  # (not the methods the engine replaces, refresh and update: those are
  # checked once, before they're replaced; after that they're the engine's)
  event: %w[find_proper_page conditions_met?],
  event_update: %w[update_stop update_self_movement near_the_screen? stop_count_threshold
                   check_event_trigger_auto update_animation update_anime_count moving? jumping? real_move_speed dash?],
  switches: %w[[]],
  variables: %w[[]],
  self_switches: %w[[]],
  sprite: %w[update_bitmap graphic_changed? update_src_rect update_position update_other update_balloon
             setup_new_effect animation? x= y= z= opacity= blend_type= bush_depth= visible= src_rect],
  character: %w[screen_x screen_y screen_z shift_y jump_height object_character? tile_id character_name
                character_index pattern direction opacity blend_type bush_depth transparent animation_id balloon_id],
  map: %w[adjust_x adjust_y loop_horizontal? loop_vertical? screen_tile_x screen_tile_y width height],
}

def mkxpz_stock?(klass, name)
  m = klass.instance_method(name)
  exp = mkxpz_stock_table["#{m.owner}##{m.original_name}"] or return false
  file, line = m.source_location
  return file.nil? if exp == :c
  return file == 'module_rpg.rb' && RubyVM::InstructionSequence.of(m).nil? if exp == :rpg
  script = mkxpz_script_text(file) or return false
  return RubyVM::InstructionSequence.of(m).nil? && script.lines[line - 1].to_s =~ /\A\s*attr_(reader|accessor)\b/ if exp == :attr
  text = mkxpz_method_text(script, line) or return false
  Zlib.crc32(mkxpz_norm(text)) == exp
rescue NameError
  false
end
# the same for RPG Maker VX (RGSS2): Sprite_Character#update (scene.c
# fast_update_vx); checksums from VX's standard scripts
MKXPZ_STOCK_VX = {
  'Sprite_Character#update' => 1579953485,
  'Sprite_Character#update_bitmap' => 3188977104,
  'Sprite_Character#update_src_rect' => 1942911822,
  'Sprite_Character#update_balloon' => 3727418189,
  'Sprite_Base#update' => 208928782,
  'Game_Character#screen_x' => 4149964556,
  'Game_Character#screen_y' => 2379379738,
  'Game_Character#screen_z' => 283499415,
  'Game_Character#object?' => 1400731312,
  'Game_Map#adjust_x' => 2112496703,
  'Game_Map#adjust_y' => 269686746,
  'Game_Map#loop_horizontal?' => 3787205086,
  'Game_Map#loop_vertical?' => 729937964,
  'Game_Character#tile_id' => :attr,
  'Game_Character#character_name' => :attr,
  'Game_Character#character_index' => :attr,
  'Game_Character#pattern' => :attr,
  'Game_Character#direction' => :attr,
  'Game_Character#opacity' => :attr,
  'Game_Character#blend_type' => :attr,
  'Game_Character#bush_depth' => :attr,
  'Game_Character#transparent' => :attr,
  'Game_Character#animation_id' => :attr,
  'Game_Character#balloon_id' => :attr,
  'Game_Event#on_screen' => 1066362925, # ("Anti Lag VX" by Anaryu)
  'Game_Map#display_x' => :attr,
  'Game_Map#display_y' => :attr,
  'RPG::Map#scroll_type' => :rpg,
  'RPG::Map#width' => :rpg,
  'RPG::Map#height' => :rpg,
  'Sprite#update' => :c,
  'Sprite#x=' => :c,
  'Sprite#y=' => :c,
  'Sprite#z=' => :c,
  'Sprite#opacity=' => :c,
  'Sprite#blend_type=' => :c,
  'Sprite#bush_depth=' => :c,
  'Sprite#visible=' => :c,
  'Sprite#src_rect' => :c,
}
MKXPZ_FAST_NEEDS_VX = {
  sprite: %w[update_bitmap update_src_rect update_balloon x= y= z= opacity= blend_type= bush_depth= visible= src_rect],
  character: %w[screen_x screen_y screen_z object? tile_id character_name character_index pattern direction opacity
                blend_type bush_depth transparent animation_id balloon_id],
  map: %w[adjust_x adjust_y loop_horizontal? loop_vertical?],
}

def mkxpz_stock_table
  $MKXPZ_RGSS == 2 ? MKXPZ_STOCK_VX : MKXPZ_STOCK
end

# a script's text by the file name Ruby reports: a section of
# Scripts.rvdata(2), or a file (games that load their scripts from files)
def mkxpz_script_text(file)
  return nil unless file
  return $mkxpz_sections[file] if $mkxpz_sections.key?(file)
  ($mkxpz_files ||= {})[file] ||= (File.exist?(file) ? File.read(file).force_encoding(Encoding::UTF_8) : false)
  $mkxpz_files[file] || nil
end

# (asked by the engine for each class it meets)
def mkxpz_fast_ok?(klass, kind)
  return false if kind == :sprite && !(klass <= Sprite_Character)
  needs = $MKXPZ_RGSS == 2 ? MKXPZ_FAST_NEEDS_VX : MKXPZ_FAST_NEEDS
  needs[kind].all? { |n| mkxpz_stock?(klass, n) }
rescue StandardError
  false
end

def mkxpz_fast_setup
  return mkxpz_fast_setup_vx if $MKXPZ_RGSS == 2
  return if $MKXPZ_RGSS != 3 || MKXPZ_STOCK.empty?
  return unless defined?(Sprite_Character) && defined?(Sprite_Base) && defined?(Game_Event) && defined?(Game_Map)
  return unless Sprite_Character.superclass == Sprite_Base && Sprite_Base.superclass == Sprite
  a = Sprite_Character.ancestors
  return unless a[1] == Sprite_Base && a[2] == Sprite
  return unless mkxpz_stock?(Sprite_Character, :update) && mkxpz_stock?(Sprite_Base, :update) && mkxpz_stock?(Sprite, :update)
  return unless mkxpz_fast_ok?(Sprite_Character, :sprite) && mkxpz_fast_ok?(Game_Event, :character) &&
                mkxpz_fast_ok?(Game_Map, :map)
  Sprite_Character.send(:alias_method, :mkxpz_slow_update, :update)
  mkxpz_fast_install(Sprite_Character)
  mkxpz_log('character sprites: native')
rescue StandardError => e
  mkxpz_log("character sprites: Ruby (#{e.class})")
end

def mkxpz_fast_setup_vx
  return unless defined?(Sprite_Character) && defined?(Sprite_Base) && defined?(Game_Event) && defined?(Game_Map)
  a = Sprite_Character.ancestors
  return unless a[1] == Sprite_Base && a[2] == Sprite
  return unless mkxpz_stock?(Sprite_Character, :update) && mkxpz_stock?(Sprite_Base, :update) && mkxpz_stock?(Sprite, :update)
  return mkxpz_log('character sprites: Ruby (not the standard VX methods)') unless
    mkxpz_fast_ok?(Sprite_Character, :sprite) && mkxpz_fast_ok?(Game_Event, :character) && mkxpz_fast_ok?(Game_Map, :map)
  Sprite_Character.send(:alias_method, :mkxpz_slow_update, :update)
  mkxpz_fast_install_vx(Sprite_Character)
  mkxpz_log('character sprites: native (VX)')
rescue StandardError => e
  mkxpz_log("character sprites: Ruby (#{e.class}: #{e.message})")
ensure
  mkxpz_fast_setup_antilag
end

# "Anti Lag VX": Game_Event#on_screen in C (events.c)
def mkxpz_fast_setup_antilag
  return unless defined?(Game_Event) && Game_Event.method_defined?(:on_screen) && defined?(Game_Map)
  return unless mkxpz_stock?(Game_Event, :on_screen) && mkxpz_stock?(Game_Map, :display_x) &&
                mkxpz_stock?(Game_Map, :display_y)
  Game_Event.send(:alias_method, :mkxpz_slow_on_screen, :on_screen)
  mkxpz_fast_install_antilag(Game_Event, $game_map ? $game_map.class : Game_Map)
  mkxpz_log('anti lag: native on_screen')
rescue StandardError => e
  mkxpz_log("anti lag: Ruby (#{e.class}: #{e.message})")
end

# Game_Event#refresh in C (events.c), with find_proper_page and
# conditions_met?, while those and the readers they use are the standard
# ones (each class is asked again by the engine: mkxpz_fast_ok?)
def mkxpz_fast_setup_events
  return if $MKXPZ_RGSS != 3 || MKXPZ_STOCK.empty? || !defined?(RPG::Event::Page::Condition)
  return unless defined?(Game_Event) && defined?(Game_Switches) && defined?(Game_Variables) && defined?(Game_SelfSwitches)
  readers = {
    RPG::Event => %w[id pages],
    RPG::Event::Page => %w[condition],
    RPG::Event::Page::Condition => %w[switch1_valid switch1_id switch2_valid switch2_id variable_valid variable_id
                                      variable_value self_switch_valid self_switch_ch item_valid actor_valid],
  }
  return unless readers.all? { |k, names| names.all? { |n| MKXPZ_STOCK["#{k}##{n}"] = :rpg; mkxpz_stock?(k, n) } }
  return unless mkxpz_stock?(Game_Event, :refresh) && mkxpz_fast_ok?(Game_Event, :event)
  Game_Event.send(:alias_method, :mkxpz_slow_refresh, :refresh)
  mkxpz_fast_install_events(Game_Event)
  mkxpz_log('events: native refresh')
  mkxpz_fast_setup_event_update
rescue StandardError => e
  mkxpz_log("events: Ruby (#{e.class})")
end

# Game_Event#update in C (scene.c) for events standing still; the super
# chain (Game_Character, Game_CharacterBase) must be the standard one too
def mkxpz_fast_setup_event_update
  return unless Game_Event.ancestors[1] == Game_Character && Game_Character.ancestors[1] == Game_CharacterBase
  return unless mkxpz_stock?(Game_Character, :update_stop) && mkxpz_stock?(Game_CharacterBase, :update_stop) &&
                mkxpz_stock?(Game_CharacterBase, :update) && Game_Character.instance_method(:update).owner == Game_CharacterBase
  return unless mkxpz_stock?(Game_Event, :update) && mkxpz_fast_ok?(Game_Event, :event_update) &&
                mkxpz_fast_ok?(Game_Map, :map)
  Game_Event.class_eval do
    alias_method :mkxpz_slow_event_update, :update
    # the end of the standard Game_Event#update (its event script)
    def mkxpz_update_interpreter
      @interpreter.setup(@list, @event.id) unless @interpreter.running?
      @interpreter.update
    end
  end
  mkxpz_fast_install_event_update(Game_Event)
  mkxpz_log('events: native update')
rescue StandardError => e
  mkxpz_log("events: Ruby update (#{e.class})")
end

# Message boxes over the whole top screen (settings: "Text boxes", with the
# fit screen mode): the engine draws the picture Graphics.mkxpz_margin wider
# on each side than the game's screen, and the message box (only it and its
# helper windows) reaches into those sides. Laid out again when the setting
# changes, once the box is closed.
# the method is boot.rb's own already (preparations applied again after a
# reset that ran the scripts again, without the game redefining it): an
# alias of it would call itself forever
def mkxpz_ours?(klass, name)
  loc = klass.instance_method(name).source_location
  loc && loc[0] == 'boot.rb'
end

def mkxpz_wide_setup
  return unless defined?(Window_Message) && Window_Message.method_defined?(:update)
  return if mkxpz_ours?(Window_Message, :update)
  Window_Message.class_eval do
    alias_method :mkxpz_wide_update, :update
    def update
      mkxpz_wide_fit
      mkxpz_wide_update
    end

    def mkxpz_wide_fit
      m = Graphics.mkxpz_margin
      return if @mkxpz_m == m || (@mkxpz_m && openness > 0) || disposed?
      old = @mkxpz_m || 0
      @mkxpz_m = m
      self.mkxpz_wide = true
      self.x = x + old - m
      self.width = width - 2 * old + 2 * m
      create_contents
      if @back_bitmap && @back_sprite && respond_to?(:create_back_bitmap, true)
        # VX Ace's dimmed background, as wide as the box
        @back_bitmap.dispose
        create_back_bitmap
        @back_sprite.bitmap = @back_bitmap
      end
      if @back_sprite
        @back_sprite.x = x
        @back_sprite.mkxpz_wide = true
      end
      @gold_window.x += m - old if @gold_window && @gold_window.x + @gold_window.width >= Graphics.width + old
      [@gold_window, @choice_window, @number_window, @item_window].each do |w|
        w.mkxpz_wide = true if w.respond_to?(:mkxpz_wide=)
      end
    rescue StandardError => e
      mkxpz_log("wide message box: #{e.class}: #{e.message}")
    end
  end
  # VX Ace's choices: at the box's right end
  if defined?(Window_ChoiceList) && Window_ChoiceList.method_defined?(:update_placement) &&
     !mkxpz_ours?(Window_ChoiceList, :update_placement)
    Window_ChoiceList.class_eval do
      alias_method :mkxpz_wide_update_placement, :update_placement
      def update_placement
        mkxpz_wide_update_placement
        self.x += Graphics.mkxpz_margin if x + width >= Graphics.width
      end
    end
  end
rescue StandardError => e
  mkxpz_log("wide message box: #{e.class}")
end

# Save probe (a file "mkxpz-savelog" in the game folder): logs each part of
# the save data as it's converted (Marshal), each map event, and each block
# written to the SD card, so a save that never finishes shows where it
# stopped (every log line is written out at once).
class MkxpzLoggedFile
  def initialize(f) @f = f; @n = 0 end
  def binmode; @f.binmode; self end
  def write(*s)
    s.each do |x|
      @n += x.bytesize
      mkxpz_log("save: writing #{x.bytesize} bytes (#{@n} in all)")
      @f.write(x)
      mkxpz_log("save: written")
    end
    s.sum(&:bytesize)
  end
end

def mkxpz_save_probe(contents)
  free = Graphics.mkxpz_speed[/free [0-9\/]+ KB/]
  mkxpz_log("save probe: #{contents.size} parts, #{free}")
  contents.each do |k, v|
    mkxpz_log("save probe: #{k} (#{v.class})...")
    if k == :map
      v.instance_variables.each do |iv|
        x = v.instance_variable_get(iv)
        mkxpz_log("save probe:   map #{iv} (#{x.class})...")
        if iv == :@events && x.is_a?(Hash)
          x.each do |id, e|
            mkxpz_log("save probe:     event #{id}...")
            mkxpz_log("save probe:     event #{id}: #{Marshal.dump(e).bytesize} bytes")
          end
        else
          mkxpz_log("save probe:   map #{iv}: #{Marshal.dump(x).bytesize} bytes")
        end
      end
    end
    mkxpz_log("save probe: #{k}: #{Marshal.dump(v).bytesize} bytes")
  end
  mkxpz_log("save probe: all parts converted")
end

def mkxpz_save_probe_setup
  return unless File.exist?('mkxpz-savelog') && defined?(DataManager) &&
                DataManager.respond_to?(:save_game_without_rescue)
  class << DataManager
    alias_method :mkxpz_probe_save, :save_game_without_rescue
    def save_game_without_rescue(index)
      mkxpz_log("save: slot #{index + 1}")
      mkxpz_save_probe(make_save_contents)
      File.open(make_filename(index), 'wb') do |file|
        f = MkxpzLoggedFile.new(file)
        $game_system.on_before_save
        Marshal.dump(make_save_header, f)
        Marshal.dump(make_save_contents, f)
        @last_savefile_index = index
      end
      mkxpz_log("save: done")
      true
    end
  end
  mkxpz_log('save probe on (mkxpz-savelog)')
rescue StandardError => e
  mkxpz_log("save probe: #{e.class}")
end

def mkxpz_run_scripts
  $RGSS_SCRIPTS = load_data($MKXPZ_SCRIPTS)
  $RGSS_SCRIPTS.each_with_index do |s, i|
    s[3] = Zlib::Inflate.inflate(s[2]).force_encoding(Encoding::UTF_8)
  end
  test = File.exist?('mkxpz-test.rb')
  # section name (as Ruby reports it) -> text; nil for names used twice
  $mkxpz_sections = {}
  $RGSS_SCRIPTS.each_with_index do |s, i|
    name = s[1].to_s.force_encoding(Encoding::UTF_8)
    file = name.empty? ? "Section#{i}" : name
    $mkxpz_sections[file] = $mkxpz_sections.key?(file) ? nil : s[3]
  end
  fast = true
  # no "Main" section (the scripts come from files through a loader, e.g.
  # Changed): the preparations at the first Graphics.update / freeze /
  # transition instead (graphics.c), when the loader has defined everything
  unless $RGSS_SCRIPTS.any? { |s| s[1].to_s.force_encoding(Encoding::UTF_8).strip == 'Main' }
    $mkxpz_late_test = test
    Graphics.mkxpz_setup_later
    fast = test = false
  end
  $RGSS_SCRIPTS.each_with_index do |s, i|
    name = s[1].to_s.force_encoding(Encoding::UTF_8)
    if fast && name.strip == 'Main'
      mkxpz_prepare
      fast = false
    end
    # for testing: mkxpz-test.rb in the game folder, before Main (which doesn't return)
    if test && name.strip == 'Main'
      eval(File.read('mkxpz-test.rb'), TOPLEVEL_BINDING, 'mkxpz-test.rb')
      test = false
    end
    eval(s[3], TOPLEVEL_BINDING, name.empty? ? "Section#{i}" : name)
  end
end

def mkxpz_prepare
  mkxpz_fast_setup unless File.exist?('mkxpz-no-fast')
  mkxpz_fast_setup_events unless File.exist?('mkxpz-no-fast') || File.exist?('mkxpz-no-fast-events')
  mkxpz_wide_setup
  mkxpz_save_probe_setup
end

def mkxpz_late_setup
  mkxpz_log("scripts from a loader: prepared at the first frame")
  mkxpz_prepare
  eval(File.read('mkxpz-test.rb'), TOPLEVEL_BINDING, 'mkxpz-test.rb') if $mkxpz_late_test
end
