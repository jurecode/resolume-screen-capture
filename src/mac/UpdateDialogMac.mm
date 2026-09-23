#include "../UpdateDialog.h"
#include "../Updater.h"
#include "../PluginIdentity.h"

#import <AppKit/AppKit.h>

namespace
{
const CGFloat WIDTH         = 470;
const CGFloat HEIGHT        = 270;
const CGFloat MARGIN        = 22;
const CGFloat BUTTON_HEIGHT = 32;

NSString* ToNSString( const std::string& text )
{
	return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}
}// namespace

@interface CSUpdateWindowController : NSObject < NSWindowDelegate >
- (instancetype)initWithCurrentVersion:(NSString*)current newVersion:(NSString*)fresh notes:(NSString*)notes;
- (void)show;
@end

static CSUpdateWindowController* openController = nil;//Keeps the window alive while it's on screen.

@implementation CSUpdateWindowController
{
	NSPanel* _window;
	NSTextField* _title;
	NSTextField* _text;
	NSButton* _install;
	NSButton* _later;
	NSButton* _skip;
	NSButton* _close;
	NSTimer* _timer;
	NSString* _newVersion;
	BOOL _installing;
}

- (NSButton*)buttonWithTitle:(NSString*)title frame:(NSRect)frame action:(SEL)action
{
	NSButton* button   = [NSButton buttonWithTitle:title target:self action:action];
	button.frame       = frame;
	button.bezelStyle  = NSBezelStyleRounded;
	[_window.contentView addSubview:button];
	return button;
}

- (instancetype)initWithCurrentVersion:(NSString*)current newVersion:(NSString*)fresh notes:(NSString*)notes
{
	if( !( self = [super init] ) )
		return nil;
	_newVersion = fresh;

	_window = [[NSPanel alloc] initWithContentRect:NSMakeRect( 0, 0, WIDTH, HEIGHT )
	                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
	                                       backing:NSBackingStoreBuffered
	                                         defer:NO];
	_window.title                = @PLUGIN_DISPLAY_NAME " - Actualización";
	_window.releasedWhenClosed   = NO;
	_window.delegate             = self;
	_window.hidesOnDeactivate    = NO;

	_title                 = [NSTextField labelWithString:@"Hay una nueva versión del plugin"];
	_title.font            = [NSFont boldSystemFontOfSize:17];
	_title.frame           = NSMakeRect( MARGIN, HEIGHT - MARGIN - 26, WIDTH - 2 * MARGIN, 26 );
	[_window.contentView addSubview:_title];

	NSString* body = [NSString stringWithFormat:@"Versión instalada: %@\nVersión nueva: %@", current, fresh];
	if( notes.length > 0 )
		body = [body stringByAppendingFormat:@"\n\nNovedades:\n%@", notes];
	_text                      = [NSTextField wrappingLabelWithString:body];
	CGFloat textTop            = HEIGHT - MARGIN - 40;
	CGFloat textBottom         = MARGIN + BUTTON_HEIGHT + 14;
	_text.frame                = NSMakeRect( MARGIN, textBottom, WIDTH - 2 * MARGIN, textTop - textBottom );
	[_window.contentView addSubview:_text];

	_install               = [self buttonWithTitle:@"Actualizar ahora" frame:NSMakeRect( MARGIN, MARGIN, 150, BUTTON_HEIGHT ) action:@selector( install: )];
	_install.keyEquivalent = @"\r";
	_later                 = [self buttonWithTitle:@"Más tarde" frame:NSMakeRect( MARGIN + 158, MARGIN, 120, BUTTON_HEIGHT ) action:@selector( later: )];
	_skip                  = [self buttonWithTitle:@"Omitir esta versión" frame:NSMakeRect( WIDTH - MARGIN - 150, MARGIN, 150, BUTTON_HEIGHT ) action:@selector( skip: )];
	_close                 = [self buttonWithTitle:@"Cerrar" frame:NSMakeRect( WIDTH - MARGIN - 150, MARGIN, 150, BUTTON_HEIGHT ) action:@selector( closeWindow: )];
	_close.hidden          = YES;
	return self;
}

- (void)show
{
	//Open on the screen the user is working on (Resolume's window), never on the projector.
	NSScreen* screen = NSApp.mainWindow.screen ?: NSApp.keyWindow.screen ?: NSScreen.mainScreen;
	NSRect area      = screen.visibleFrame;
	[_window setFrameOrigin:NSMakePoint( NSMidX( area ) - WIDTH / 2, NSMidY( area ) - HEIGHT / 2 )];
	[_window makeKeyAndOrderFront:nil];
}

- (void)showButtonsInstall:(BOOL)install later:(BOOL)later skip:(BOOL)skip close:(BOOL)close
{
	_install.hidden = !install;
	_later.hidden   = !later;
	_skip.hidden    = !skip;
	_close.hidden   = !close;
}

- (void)install:(id)sender
{
	_installing = YES;
	Updater::Get().PressAction();
	_title.stringValue = @"Actualizando…";
	_text.stringValue  = [NSString stringWithFormat:@"Descargando e instalando la versión %@.\n\nPuedes seguir usando Arena mientras tanto.", _newVersion];
	[self showButtonsInstall:NO later:NO skip:NO close:NO];
	[_timer invalidate];
	_timer = [NSTimer scheduledTimerWithTimeInterval:0.3 target:self selector:@selector( poll: ) userInfo:nil repeats:YES];
}

- (void)poll:(NSTimer*)timer
{
	Updater::Status status = Updater::Get().GetStatus();
	switch( status.state )
	{
	case Updater::State::Installed:
		[_timer invalidate];
		_installing        = NO;
		_title.stringValue = @"¡Listo!";
		_text.stringValue  = [NSString stringWithFormat:@"La versión %@ quedó instalada.\n\nCierra Resolume Arena y vuelve a abrirlo para empezar a usarla.", _newVersion];
		_close.title       = @"Entendido";
		[self showButtonsInstall:NO later:NO skip:NO close:YES];
		break;

	case Updater::State::Failed:
		[_timer invalidate];
		_installing        = NO;
		_title.stringValue = @"No se pudo actualizar";
		_text.stringValue  = [ToNSString( status.error ) stringByAppendingString:@"\n\nTu versión actual sigue funcionando normalmente."];
		_install.title     = @"Reintentar";
		[self showButtonsInstall:YES later:NO skip:NO close:YES];
		break;

	case Updater::State::Available:
	case Updater::State::Postponed:
		//"Reintentar" first checks again; once the update is found, go on and install it.
		if( _installing )
			Updater::Get().PressAction();
		break;

	default:
		break;
	}
}

- (void)later:(id)sender
{
	Updater::Get().RemindLater();
	[_window close];
}

- (void)skip:(id)sender
{
	Updater::Get().SkipVersion();
	[_window close];
}

- (void)closeWindow:(id)sender
{
	[_window close];
}

- (void)windowWillClose:(NSNotification*)notification
{
	//Closing the window while it downloads is fine: the install carries on in the background.
	[_timer invalidate];
	_timer         = nil;
	openController = nil;
}
@end

void ShowUpdateDialog( const std::string& currentVersion, const std::string& newVersion, const std::string& notes )
{
	NSString* current = ToNSString( currentVersion );
	NSString* fresh   = ToNSString( newVersion );
	NSString* text    = ToNSString( notes );
	dispatch_async( dispatch_get_main_queue(), ^{
		if( openController != nil )
			return;
		openController = [[CSUpdateWindowController alloc] initWithCurrentVersion:current newVersion:fresh notes:text];
		[openController show];
	} );
}
